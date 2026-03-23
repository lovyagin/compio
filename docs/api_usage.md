# API Usage Guide

## Compression Algorithms

The library supports multiple compression algorithms:

- **NONE (Dummy)** - No compression, data stored as-is
- **ZLIB** - Standard compression (default)
- **LZ4** - Very fast compression/decompression
- **Zstandard (zstd)** - Modern efficient compression
- **Brotli** - High compression ratio

### Selecting Compression Algorithm

When creating an archive, you can select compression algorithm:

```c
#include "compio.h"

// Create configuration
compio_config config;
compio_build_default_config(&config);

// Select compression algorithm
compio_build_lz4_compressor(&config.compressor);      // LZ4
// compio_build_zstd_compressor(&config.compressor);  // Zstandard
// compio_build_brotli_compressor(&config.compressor); // Brotli
// compio_build_zlib_compressor(&config.compressor);  // ZLIB (default)
// compio_build_dummy_compressor(&config.compressor); // No compression

// Create archive with selected compressor
compio_archive* archive = compio_open_archive("archive.cmp", "w+", &config);
```

### Automatic Compression Detection

The compression algorithm is automatically saved in the archive header. When reopening an archive, the correct compressor is automatically selected:

```c
// Open existing archive - compressor auto-detected from header
compio_config config;
compio_build_default_config(&config);  // Any config works
compio_archive* archive = compio_open_archive("archive.cmp", "r+", &config);
// Archive will use the same compressor it was created with
```

### Basic Operations

```c
// Open archive
compio_archive* archive = compio_open_archive("archive.cmp", "w+", &config);

// Open file inside archive
compio_file* file = compio_open_file("myfile.txt", archive);

// Write data
const char* data = "Hello, World!";
compio_write(data, strlen(data) + 1, file);

// Read data
char buffer[256];
compio_seek(file, 0, COMPIO_SEEK_SET);
compio_read(buffer, sizeof(buffer), file);

// Close file and archive
compio_close_file(file);
compio_close_archive(archive);
```

### Configuration Options

```c
compio_config config;
config.b_tree_degree = 16;                          // B-Tree degree
config.block_size = 4096;                           // Block size in bytes
config.block_size__minimum = 512;                   // Minumum block size in bytes
config.block_size__maximum = 16384;                 // Maximum block size in bytes
config.cache_size__nodes = 1024;                    // B-tree node cache
config.cache_size__blocks = 8192;                   // Storage block cache
config.allocation_strategy = COMPIO_ALLOC_FIRST_FIT; // Allocation strategy
config.fragmentation_threshold = 30;                // Defrag threshold (%)
config.fill_holes_with_zeros = false;               // Zero-fill freed blocks
config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;      // WAL synchronization mode (ALWAYS, NORMAL, OFF)
config.wal_max_size_bytes = 64 * 1024 * 1024;       // Max WAL size before checkpoint
config.max_files = 4096;                            // Initial file table capacity (grows dynamically)
```

### Allocation Strategies

- `COMPIO_ALLOC_FIRST_FIT` - Use first suitable free block
- `COMPIO_ALLOC_BEST_FIT` - Use smallest suitable free block
- `COMPIO_ALLOC_WORST_FIT` - Use largest suitable free block
- `COMPIO_ALLOC_NEXT_FIT` - Continue from last allocation position

### Thread Safety

The library is thread-safe for concurrent operations on the same archive:
- **Concurrent Reads**: Multiple threads can read from different files (or same file) concurrently.
- **Concurrent Writes**: Multiple threads can write to different files concurrently.
- **Concurrent Read/Write**: Readers and writers can operate concurrently.
- **Defragmentation**: Can run concurrently with other operations (locks only necessary regions).

Note: `compio_config` setup is not thread-safe; initialize it before opening archive.

### Tools

#### compio_repair

A CLI tool to recover data from corrupted archives.

```bash
# Basic usage
./compio_repair input_archive.cmp output_directory/

# It attempts to recover files even if the header or index is corrupted.
```

#### compio_unpack

A CLI tool to extract all files from a valid archive.

```bash
# Basic usage
./compio_unpack input_archive.cmp output_directory_prefix/

# Extracts all files from the archive to the specified location.
# Note: output_directory_prefix is a prefix, so if you want a directory, end it with /
# Example: ./compio_unpack archive.cmp extracted/
```
