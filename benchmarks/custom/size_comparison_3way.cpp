#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

#include "compio.h"
#include "sample_data.hpp"
#include "zstd_seekable.h"

namespace {

constexpr std::size_t kSampleSize = sizeof(html_data) - 1; // skip string terminator

std::string make_temp_path(const char* suffix) {
    static std::minstd_rand rng(0);
    static std::uniform_int_distribution<int> dist(10000, 99999);
    std::string path;
    do {
        path = "size_cmp_" + std::to_string(dist(rng)) + suffix;
    } while (std::filesystem::exists(path));
    return path;
}

void remove_if_exists(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void fill_chunk_from_sample(char* dst, std::size_t len, std::size_t& sample_pos) {
    std::size_t written = 0;
    while (written < len) {
        if (sample_pos >= kSampleSize) {
            sample_pos = 0;
        }
        const std::size_t available = kSampleSize - sample_pos;
        const std::size_t to_copy = std::min(available, len - written);
        std::memcpy(dst + written, html_data + sample_pos, to_copy);
        sample_pos += to_copy;
        written += to_copy;
    }
}

void write_compio_archive(const std::string& archive_path, std::size_t payload_size) {
    compio_config config;
    compio_build_default_config(&config);
    compio_build_zstd_compressor(&config.compressor);
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;

    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    if (!archive) {
        throw std::runtime_error("compio_open_archive failed");
    }

    compio_file* file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        throw std::runtime_error("compio_open_file failed");
    }

    constexpr std::size_t chunk_size = 1u << 16;
    std::vector<char> buffer(chunk_size);
    std::size_t sample_pos = 0;
    std::size_t written_total = 0;
    while (written_total < payload_size) {
        const std::size_t chunk = std::min(chunk_size, payload_size - written_total);
        fill_chunk_from_sample(buffer.data(), chunk, sample_pos);

        const uint64_t written = compio_write(buffer.data(), chunk, file);
        if (written != chunk) {
            compio_close_file(file);
            compio_close_archive(archive);
            throw std::runtime_error("compio_write failed");
        }
        written_total += chunk;
    }

    compio_close_file(file);
    compio_close_archive(archive);
}

void write_gzip_with_flush_points(const std::string& path, std::size_t payload_size,
                                  std::size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb");
    if (!gz) {
        throw std::runtime_error("gzopen failed");
    }

    constexpr std::size_t chunk_size = 1u << 15;
    std::vector<char> buffer(chunk_size);
    std::size_t sample_pos = 0;
    std::size_t written_total = 0;
    std::size_t bytes_since_flush = 0;
    while (written_total < payload_size) {
        const std::size_t chunk = std::min(chunk_size, payload_size - written_total);
        fill_chunk_from_sample(buffer.data(), chunk, sample_pos);

        const int written = gzwrite(gz, buffer.data(), static_cast<unsigned>(chunk));
        if (written != static_cast<int>(chunk)) {
            int err_no = Z_OK;
            const char* err = gzerror(gz, &err_no);
            gzclose(gz);
            throw std::runtime_error(std::string("gzwrite failed: ") + (err ? err : "unknown"));
        }

        written_total += chunk;
        bytes_since_flush += chunk;
        if (bytes_since_flush >= flush_interval) {
            if (gzflush(gz, Z_FULL_FLUSH) != Z_OK) {
                int err_no = Z_OK;
                const char* err = gzerror(gz, &err_no);
                gzclose(gz);
                throw std::runtime_error(std::string("gzflush failed: ") + (err ? err : "unknown"));
            }
            bytes_since_flush = 0;
        }
    }

    if (gzclose(gz) != Z_OK) {
        throw std::runtime_error("gzclose failed");
    }
}

void write_seekable_zstd_file(const std::string& path, std::size_t payload_size,
                              unsigned max_frame_size) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) {
        throw std::runtime_error("fopen failed");
    }

    ZSTD_seekable_CStream* stream = ZSTD_seekable_createCStream();
    if (!stream) {
        std::fclose(out);
        throw std::runtime_error("ZSTD_seekable_createCStream failed");
    }

    size_t ret = ZSTD_seekable_initCStream(stream, 5, 1, max_frame_size);
    if (ZSTD_isError(ret)) {
        ZSTD_seekable_freeCStream(stream);
        std::fclose(out);
        throw std::runtime_error(std::string("ZSTD_seekable_initCStream failed: ") +
                                 ZSTD_getErrorName(ret));
    }

    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    std::vector<char> in_buffer(1u << 15);
    std::size_t sample_pos = 0;
    std::size_t written_total = 0;
    while (written_total < payload_size) {
        const std::size_t chunk = std::min(in_buffer.size(), payload_size - written_total);
        fill_chunk_from_sample(in_buffer.data(), chunk, sample_pos);
        ZSTD_inBuffer input{in_buffer.data(), chunk, 0};

        while (input.pos < input.size) {
            ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &output, &input);
            if (ZSTD_isError(ret)) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error(std::string("ZSTD_seekable_compressStream failed: ") +
                                         ZSTD_getErrorName(ret));
            }
            if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error("fwrite failed");
            }
        }
        written_total += chunk;
    }

    do {
        ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &output);
        if (ZSTD_isError(ret)) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error(std::string("ZSTD_seekable_endStream failed: ") +
                                     ZSTD_getErrorName(ret));
        }
        if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error("fwrite failed");
        }
    } while (ret != 0);

    ZSTD_seekable_freeCStream(stream);
    std::fclose(out);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t payload_size =
            argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : ((1u << 30) + (64u << 20));
        constexpr std::size_t access_granularity = 1u << 13; // 8 KiB

        const std::string compio_path = make_temp_path(".compio");
        const std::string gzip_path = make_temp_path(".gz");
        const std::string seekable_path = make_temp_path(".seek.zst");

        write_compio_archive(compio_path, payload_size);
        write_gzip_with_flush_points(gzip_path, payload_size, access_granularity);
        write_seekable_zstd_file(seekable_path, payload_size, static_cast<unsigned>(access_granularity));

        const auto compio_size = std::filesystem::file_size(compio_path);
        const auto zran_size = std::filesystem::file_size(gzip_path);
        const auto seekable_size = std::filesystem::file_size(seekable_path);

        std::cout << "Payload size (bytes): " << payload_size << "\n";
        std::cout << "| Backend | File size (bytes) |\n";
        std::cout << "|---|---:|\n";
        std::cout << "| compio (zstd) | " << compio_size << " |\n";
        std::cout << "| zran (gzip + full flush 8KiB) | " << zran_size << " |\n";
        std::cout << "| seekable zstd (frame 8KiB) | " << seekable_size << " |\n";

        remove_if_exists(compio_path);
        remove_if_exists(compio_path + ".wal");
        remove_if_exists(gzip_path);
        remove_if_exists(seekable_path);

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
