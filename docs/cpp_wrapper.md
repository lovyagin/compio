# C++ Wrapper API

## Overview

Modern C++ interface with RAII, stream operators, and type safety. Header-only wrapper over C API.

## Quick Start

```cpp
#include "compio.hpp"

compio::Archive archive("data.compio", "w+");
auto file = archive.open_file("data.bin");

int value = 42;
file << value;  // Write
file.seek(0, COMPIO_SEEK_SET);
file >> value;  // Read
```

## API Reference

### Config

Fluent configuration builder:

```cpp
compio::Config config;
config.set_block_size(4096)
      .set_btree_degree(5)
      .set_compressor(compio::compressors::lz4());
```

**Methods**: `set_block_size()`, `set_btree_degree()`, `set_cache_nodes()`, `set_cache_blocks()`, `set_compressor()`, `set_allocation_strategy()`, `set_fill_holes()`, `set_fragmentation_threshold()`

### Archive

RAII archive management:

```cpp
compio::Archive archive("file.compio", "w+", config);
auto file = archive.open_file("name.bin");
archive.remove_file("old.bin");
archive.flush();
```

### File

Stream-based I/O:

```cpp
// Stream operators (any trivially copyable type)
file << value;
file >> value;

// Raw I/O
file.write(data, size);
file.read(buffer, size);

// Positioning
file.seek(offset, origin);
uint64_t pos = file.tell();
```

## Supported Types

Works with any POD type:
```cpp
struct Point { double x, y, z; };
Point p{1.0, 2.0, 3.0};
file << p;  // Binary serialization
```

## Compressors

```cpp
compio::compressors::dummy();   // None
compio::compressors::zlib();    // Standard
compio::compressors::lz4();     // Fast
compio::compressors::zstd();    // Balanced
compio::compressors::brotli();  // High ratio
```

## Error Handling

```cpp
try {
    compio::Archive archive("file.compio", "w+");
    // ...
} catch (const compio::Exception& e) {
    std::cerr << e.what() << std::endl;
}
```

## Example

See `examples/cpp_wrapper_example.cpp` for complete usage demonstration.
