# Compression Examples

This directory contains examples demonstrating compression functionality.

## Examples

### `example_usage.c`
Basic end-to-end example: open archive, write data, read it back, close.

**Usage:**
```bash
./example_usage
```

### `compressor_example.c`
Demonstrates all available compression algorithms and their compression ratios on sample data.

**Usage:**
```bash
./compressor_example
```

**Output:**
Shows compression ratios for Dummy, ZLIB, LZ4, Zstandard, and Brotli algorithms.

### `compression_persistence_test.c`
Demonstrates that compression type is automatically saved and restored when reopening archives.

**Usage:**
```bash
./compression_persistence_test
```

**Features:**
- Creates archives with different compressors (LZ4, Zstandard, Brotli)
- Closes and reopens each archive
- Verifies data integrity and correct decompression

This example proves that you don't need to specify the compression algorithm when opening an existing archive - it's automatically detected from the header.

### `cpp_wrapper_example.cpp`
Demonstrates the C++ wrapper API: RAII archive/file management, stream operators, and error handling.

**Usage:**
```bash
./cpp_wrapper_example
```

### `compio_repair.cpp`
Minimal recovery example: calls `compio_repair()` to salvage files from a corrupted archive into an output directory.

**Usage:**
```bash
./repair_example <archive_path> <output_dir>
```

### `compio_unpack.cpp`
Minimal extraction example: opens a healthy archive and streams every file into an output directory in fixed-size chunks (memory use independent of file size).

**Usage:**
```bash
./unpack_example <archive_path> <output_dir>
```

> These are stripped-down demos. The full-featured command-line tools — with `--help`, prefix/directory modes, `--force`, and path-traversal guards — live in [`util/`](../util/README.md).

## Available Compression Algorithms

| Algorithm | Speed | Ratio | Use Case |
|-----------|-------|-------|----------|
| **Dummy** | Fastest | None | Testing, uncompressed storage |
| **LZ4** | Very Fast | Good | Real-time compression |
| **ZLIB** | Fast | Good | General purpose (default) |
| **Zstandard** | Fast | Excellent | Modern applications |
| **Brotli** | Moderate | Best | Maximum compression |

## Quick Start

```c
#include "compio.h"

// Select compression algorithm
compio_config config;
compio_build_default_config(&config);
compio_build_lz4_compressor(&config.compressor);  // or zstd, brotli, zlib, dummy

// Create archive
compio_archive* archive = compio_open_archive("file.cmp", "w+", &config);

// Use archive...

// Close archive (compression type is saved automatically)
compio_close_archive(archive);

// Reopen - compression type auto-detected!
archive = compio_open_archive("file.cmp", "r+", &config);
```

