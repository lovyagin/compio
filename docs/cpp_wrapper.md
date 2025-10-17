# C++ Wrapper API

## Overview

The C++ wrapper (`compio.hpp`) provides a modern, type-safe interface to the compio library with RAII resource management and stream-style I/O operators.

## Key Features

- **RAII Resource Management**: Automatic cleanup of archives and files
- **Stream-style I/O**: Overloaded `<<` and `>>` operators for binary I/O
- **Type Safety**: Template-based binary serialization for trivially copyable types
- **Exception Handling**: C++ exceptions instead of error codes
- **Modern C++11**: Uses move semantics and smart pointers

## Quick Start

```cpp
#include "compio.hpp"

// Create and write to archive
compio::Archive archive("data.compio", "w+");
auto file = archive.open_file("data.bin");

int value = 42;
double pi = 3.14159;

// Stream-style binary write
file << value << pi;

// Read back
file.seek(0, COMP_SEEK_SET);
int read_value;
double read_pi;
file >> read_value >> read_pi;
```

## Classes

### `compio::Config`

Configuration builder with fluent interface:

```cpp
compio::Config config;
config.set_block_size(4096)
      .set_btree_degree(5)
      .set_compressor(compio::compressors::lz4())
      .set_cache_blocks(100);
```

### `compio::Archive`

RAII wrapper for archive operations:

```cpp
// With default config
compio::Archive archive("file.compio", "w+");

// With custom config
compio::Archive archive("file.compio", "w+", config);

// Open file in archive
auto file = archive.open_file("myfile.bin");

// Remove file
archive.remove_file("oldfile.bin");

// Flush to disk
archive.flush();
```

### `compio::File`

File operations with stream interface:

```cpp
auto file = archive.open_file("data.bin");

// Binary write/read with operators
int32_t a = 42;
file << a;

// Read back
file.seek(0, COMP_SEEK_SET);
int32_t b;
file >> b;

// Raw operations
const char* data = "hello";
file.write(data, 5);

char buffer[5];
file.read(buffer, 5);

// Position operations
file.seek(100, COMP_SEEK_SET);
uint64_t pos = file.tell();
```

## Supported Types

Stream operators (`<<` and `>>`) work with any trivially copyable type:
- Fundamental types: `int`, `float`, `double`, etc.
- POD structures
- Arrays of trivially copyable types

```cpp
struct Point {
    double x, y, z;
};

Point p{1.0, 2.0, 3.0};
file << p;  // Binary write

Point p2;
file >> p2;  // Binary read
```

## Compressor Helpers

```cpp
namespace compio::compressors {
    compio_compressor dummy();   // No compression
    compio_compressor zlib();    // ZLIB compression
    compio_compressor lz4();     // LZ4 (fast)
    compio_compressor zstd();    // Zstandard (efficient)
    compio_compressor brotli();  // Brotli (high ratio)
}
```

## Exception Handling

```cpp
try {
    compio::Archive archive("file.compio", "w+");
    auto file = archive.open_file("data.bin");
    // ... operations
} catch (const compio::Exception& e) {
    std::cerr << "Compio error: " << e.what() << std::endl;
}
```

## Complete Example

See `examples/cpp_wrapper_example.cpp` for a comprehensive demonstration of all features.

