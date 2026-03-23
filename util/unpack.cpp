#include <stdexcept>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <string>
#include <filesystem>
#include <cinttypes>

#include "compio/compio_file.hpp"
#include "compio.h"

namespace fs = std::filesystem;

// Helper to sanitize and join paths, preventing traversal attacks
fs::path safe_join(const fs::path& base, const std::string& part) {
    fs::path target = base / part;
    // Normalize path
    target = target.lexically_normal();
    
    // Check if target is within base
    // Note: lexically_normal() handles ".." resolution. 
    // We convert both to absolute paths to be sure, or just check the string prefix if base is absolute.
    // However, if base is relative, we need to be careful.
    // A simple check: ensure the resulting path starts with the base path.
    // But lexically_normal might produce "base/../other" -> "other", which is outside.
    // So we check if the relative path from base to target starts with "..".
    
    // Ensure base is absolute for robust checking
    fs::path abs_base = fs::absolute(base);
    fs::path abs_target = fs::absolute(target);
    
    auto rel = fs::relative(abs_target, abs_base);
    if (!rel.empty() && rel.string().rfind("..", 0) == 0) {
        throw std::runtime_error("security error: path traversal detected: " + part);
    }
    
    return target;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        throw std::runtime_error("usage: ./compio_unpack <file> <output_prefix>");
    }

    const std::string out_prefix_str = argv[2];
    fs::path out_prefix(out_prefix_str);

    // If prefix ends with separator, treat it as a directory
    bool is_directory_mode = !out_prefix_str.empty() && 
                             (out_prefix_str.back() == fs::path::preferred_separator || 
                              out_prefix_str.back() == '/'); // support both slash types

    if (is_directory_mode) {
        std::error_code ec;
        fs::create_directories(out_prefix, ec);
        if (ec) {
             throw std::runtime_error("failed to create directory " + out_prefix.string());
        }
    } else if (out_prefix.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out_prefix.parent_path(), ec);
        if (ec) {
             throw std::runtime_error("failed to create directory " + out_prefix.parent_path().string());
        }
    }

    compio_config config;
    compio_build_default_config(&config);

    compio_archive *archive = compio_open_archive(argv[1], "r", &config);
    if (!archive) {
        throw std::runtime_error("failed to open archive");
    }

    const auto ftable = &archive->header->ftable;
    if (ftable->n_files == 0) {
        throw std::runtime_error("archive has no files");
    }

    for (std::size_t i = 0; i < ftable->n_files; ++i) {
        const auto &f = ftable->files[i];
        printf("reading file %s\n", f.name);

        compio_file *file = compio_open_file(f.name, archive);
        if (!file) {
            throw std::runtime_error("failed to open file " + std::string(f.name));
        }

        printf("size = %" PRIu64 "\n", f.size);

        // Sanitize output path
        fs::path out_fp;
        if (is_directory_mode) {
            out_fp = safe_join(out_prefix, f.name);
        } else {
             // If out_prefix is just a prefix string (e.g. "out_"), we just concat.
             // But we still need to check for traversal in f.name itself.
             // This is tricky because "out_" + "../foo" -> "out_../foo".
             // Safer to only support directory mode or simple prefix.
             // But to preserve backward compatibility (if any), let's assume simple concat is allowed
             // providing f.name doesn't traverse up.
             
             // Construct the full path first
             std::string full_path_str = out_prefix_str + f.name;
             fs::path full_path(full_path_str);
             
             // Check if full_path is safe relative to CWD? 
             // Or relative to the parent of out_prefix?
             // Let's enforce that f.name does not contain ".."
             fs::path name_path(f.name);
             if (name_path.is_absolute() || name_path.string().find("..") != std::string::npos) {
                 throw std::runtime_error("security error: invalid filename " + std::string(f.name));
             }
             out_fp = full_path;
        }

        // Ensure parent directory of the output file exists
        if (out_fp.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(out_fp.parent_path(), ec);
            if (ec) {
                compio_close_file(file);
                throw std::runtime_error("failed to create directory " + out_fp.parent_path().string());
            }
        }

        FILE *out_file = fopen(out_fp.string().c_str(), "wb");
        if (!out_file) {
            compio_close_file(file);
            throw std::runtime_error("failed to open output file " + out_fp.string());
        }

        // Use 1MB buffer to avoid huge allocations
        constexpr size_t buffer_size = 1024 * 1024;
        auto buffer = std::make_unique<uint8_t[]>(buffer_size);
        
        uint64_t remaining = f.size;
        while (remaining > 0) {
            size_t to_read = std::min(static_cast<uint64_t>(buffer_size), remaining);
            size_t read_bytes = compio_read(buffer.get(), to_read, file);
            
            if (read_bytes != to_read) {
                fclose(out_file);
                compio_close_file(file);
                throw std::runtime_error("failed to read file " + std::string(f.name));
            }
            
            if (fwrite(buffer.get(), 1, read_bytes, out_file) != read_bytes) {
                fclose(out_file);
                compio_close_file(file);
                throw std::runtime_error("failed to write data to file " + out_fp.string());
            }
            remaining -= read_bytes;
        }

        fclose(out_file);

        compio_close_file(file);
    }

    compio_close_archive(archive);

    return 0;
}
