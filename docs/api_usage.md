# API Usage Guide

## Compression Algorithms

The library supports multiple compression algorithms:

- **NONE (Dummy)** - No compression, data stored as-is
- **ZLIB** - Standard compression
- **LZ4** - Very fast compression/decompression (default)
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
compio_build_lz4_compressor(&config.compressor);      // LZ4 (default)
// compio_build_zstd_compressor(&config.compressor);  // Zstandard
// compio_build_brotli_compressor(&config.compressor); // Brotli
// compio_build_zlib_compressor(&config.compressor);  // ZLIB
// compio_build_dummy_compressor(&config.compressor); // No compression

// Create archive with selected compressor
compio_archive* archive = compio_open_archive("archive.cmp", "w+", &config);
```

### Selecting Compression Level

Each algorithm supports an explicit level via `_with_level` variants:

```c
// ZLIB: 0 = no compression, 1 = fastest, 9 = best; default = Z_DEFAULT_COMPRESSION
compio_build_zlib_compressor_with_level(&config.compressor, 9);

// LZ4: acceleration factor — 1 = default balance, higher = faster/lower ratio
compio_build_lz4_compressor_with_level(&config.compressor, 1);

// Zstandard: 1 = fastest, 22 = best compression; default = 5
compio_build_zstd_compressor_with_level(&config.compressor, 3);

// Brotli: 0 = fastest, 11 = best compression; default = 5
compio_build_brotli_compressor_with_level(&config.compressor, 11);
```

### Reopening an Archive

The archive header records the compressor identifier (`compression_type`). Opening an archive
with a compressor of another identifier fails with `EINVAL`, so blocks are never decoded by the
wrong algorithm. For built-in compressors the matching one can be built from the header:

```c
compio_compression_type type;
compio_get_compression_type("archive.cmp", &type);

compio_config config;
compio_build_default_config(&config);
if (compio_build_compressor_by_type(&config.compressor, type) != COMPIO_SUCCESS) {
    // ENOTSUP: the archive uses a custom compressor, supply it yourself
}
compio_archive* archive = compio_open_archive("archive.cmp", "r+", &config);
```

`block_size` and `b_tree_degree` are recorded in the header too. Set them to 0 to take the
values from the archive; any other value must match, otherwise opening fails with `EINVAL`.

### Custom Compressors

A custom compressor fills `compress`, `decompress` and `get_bufsize` and takes its own
identifier from `COMPIO_COMPRESS_CUSTOM` up to `INT32_MAX`. Give each algorithm, and each
incompatible version of one, a distinct identifier: the identifier is the only thing that tells
two external codecs apart, because block checksums cover the compressed bytes.

```c
config.compressor.compress = my_compress;
config.compressor.decompress = my_decompress;
config.compressor.get_bufsize = my_bufsize;
config.compressor.compression_type = (compio_compression_type)(COMPIO_COMPRESS_CUSTOM + 1);
```

`compio_repair` decodes blocks only with built-in compressors and refuses archives that use a
custom one.

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
compio_build_default_config(&config);       // Initialize with defaults first!

// The values below are the defaults set by compio_build_default_config.
config.b_tree_degree = 16;                          // B-Tree degree
config.block_size = 16384;                          // Target block size in bytes
config.block_size__minimum = 512;                   // Minimum block size in bytes
config.block_size__maximum = 262144;                // Maximum block size in bytes (>= 2 * block_size)
config.cache_size__nodes = 1024;                    // B-tree node cache, in nodes
config.cache_size__blocks = 8192;                   // Block cache, in blocks, shared by all files
config.allocation_strategy = COMPIO_ALLOC_BEST_FIT; // Allocation strategy
config.fragmentation_threshold = 30;                // Compact when free regions exceed this share (%) of the archive
config.fill_holes_with_zeros = false;               // Return freed regions to the filesystem (hole punching)
config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;      // WAL synchronization mode (ALWAYS, NORMAL, OFF)
config.wal_max_size_bytes = 64 * 1024 * 1024;       // Max WAL size before checkpoint
config.max_files = 4096;                            // Expected number of files (a hint, not a limit)
config.checksum_type = COMPIO_CHECKSUM_CRC32C;      // Checksum algorithm (FNV1A or CRC32C)
config.auto_batch_size = 8;                         // Auto-batch sequential ops (0 = disabled)
config.enable_wal = true;                           // Maintain the write-ahead log
```

Archive open modes are `"r"`, `"r+"`, `"w"`, `"w+"`, `"a"`, `"a+"` (an optional `b` is
ignored); any other string is rejected with `EINVAL`. File names are 1 to
`COMPIO_FNAME_MAX_SIZE - 1` (4095) bytes long and may contain `/`; a longer name is
rejected with `ENAMETOOLONG`, an empty one with `EINVAL`.

### Allocation Strategies

- `COMPIO_ALLOC_FIRST_FIT` - Use first suitable free block
- `COMPIO_ALLOC_BEST_FIT` - Use smallest suitable free block (default)
- `COMPIO_ALLOC_WORST_FIT` - Use largest suitable free block
- `COMPIO_ALLOC_NEXT_FIT` - Continue from last allocation position

### Thread Safety

The library is thread-safe for multi-threaded use on the same archive, with the following guarantees:
- **Concurrent reads**: Multiple threads can read from different files (or the same file) concurrently using separate file handles.
- **Concurrent writes**: Multiple threads can write to **different** files simultaneously, each using its own file handle.
- **Per-file exclusivity**: A single file handle must not be used by multiple threads concurrently — each thread needs its own handle (via `compio_open_file`).
- **Defragmentation is exclusive**: `compio_defragment` cannot run concurrently with reads or writes on that archive.

Note: `compio_config` setup is not thread-safe; initialize it before opening an archive.

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
