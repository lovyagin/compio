#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include "compio.h"

// Simple argument parser
struct Args {
    std::string archive_path;
    std::string output_dir;
    bool help = false;
};

void print_usage(const char* prog_name) {
    printf("Usage: %s <archive_path> <output_dir>\n", prog_name);
    printf("Recover files from a corrupted compio archive.\n\n");
    printf("Arguments:\n");
    printf("  archive_path  Path to the corrupted archive file\n");
    printf("  output_dir    Directory where recovered files will be saved\n");
}

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc != 3) {
        args.help = true;
        return args;
    }
    args.archive_path = argv[1];
    args.output_dir = argv[2];
    return args;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    
    if (args.help) {
        print_usage(argv[0]);
        return 1;
    }

    if (!std::filesystem::exists(args.archive_path)) {
        fprintf(stderr, "Error: Archive file '%s' does not exist.\n", args.archive_path.c_str());
        return 1;
    }

    // Ensure output directory exists
    try {
        std::filesystem::create_directories(args.output_dir);
    } catch (const std::filesystem::filesystem_error& e) {
        fprintf(stderr, "Error creating output directory: %s\n", e.what());
        return 1;
    }

    printf("Attempting to repair archive: %s\n", args.archive_path.c_str());
    printf("Output directory: %s\n", args.output_dir.c_str());

    int recovered_count = compio_repair(args.archive_path.c_str(), args.output_dir.c_str());

    if (recovered_count >= 0) {
        printf("\nSuccess! Recovered %d file(s) to '%s'.\n", recovered_count, args.output_dir.c_str());
        return 0;
    } else {
        fprintf(stderr, "\nRepair failed. See warnings/errors above for details.\n");
        return 1;
    }
}
