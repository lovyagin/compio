#include <cstdio>
#include <string>
#include <filesystem>
#include <system_error>
#include "compio.h"

// Simple argument parser
struct Args {
    std::string archive_path;
    std::string output_dir;
    bool help = false;
    bool force = false;
};

void print_usage(const char* prog_name) {
    printf("Usage: %s <archive_path> <output_dir> [--force]\n", prog_name);
    printf("Recover files from a corrupted compio archive.\n\n");
    printf("Arguments:\n");
    printf("  archive_path  Path to the corrupted archive file\n");
    printf("  output_dir    Directory where recovered files will be saved\n");
    printf("  --force       Overwrite existing files in output directory if present\n");
}

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc < 3) {
        args.help = true;
        return args;
    }
    args.archive_path = argv[1];
    args.output_dir = argv[2];
    
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--force" || arg == "-f") {
            args.force = true;
        } else if (arg == "--help" || arg == "-h") {
            args.help = true;
        }
    }
    return args;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    
    if (args.help) {
        print_usage(argv[0]);
        return 1;
    }

    std::error_code ec;
    bool exists = std::filesystem::exists(args.archive_path, ec);
    if (ec) {
        fprintf(stderr, "Error checking archive file '%s': %s\n", args.archive_path.c_str(), ec.message().c_str());
        return 1;
    }
    if (!exists) {
        fprintf(stderr, "Error: Archive file '%s' does not exist.\n", args.archive_path.c_str());
        return 1;
    }

    // Check output directory safety
    bool out_exists = std::filesystem::exists(args.output_dir, ec);
    if (ec) {
        fprintf(stderr, "Error checking output directory '%s': %s\n", args.output_dir.c_str(), ec.message().c_str());
        return 1;
    }
    
    if (out_exists && !args.force) {
        if (!std::filesystem::is_empty(args.output_dir, ec)) {
             fprintf(stderr, "Error: Output directory '%s' is not empty.\n", args.output_dir.c_str());
             fprintf(stderr, "Use --force to overwrite existing files.\n");
             return 1;
        }
    }

    // Ensure output directory exists (create if missing)
    if (!out_exists) {
        if (!std::filesystem::create_directories(args.output_dir, ec)) {
            if (ec) {
                fprintf(stderr, "Error creating output directory: %s\n", ec.message().c_str());
                return 1;
            }
        }
    }

    printf("Attempting to repair archive: %s\n", args.archive_path.c_str());
    printf("Output directory: %s\n", args.output_dir.c_str());
    if (args.force) {
        printf("Force mode enabled: existing files may be overwritten.\n");
    }

    int recovered_count = compio_repair(args.archive_path.c_str(), args.output_dir.c_str());

    if (recovered_count >= 0) {
        printf("\nSuccess! Recovered %d file(s) to '%s'.\n", recovered_count, args.output_dir.c_str());
        return 0;
    } else {
        fprintf(stderr, "\nRepair failed. See warnings/errors above for details.\n");
        return 1;
    }
}
