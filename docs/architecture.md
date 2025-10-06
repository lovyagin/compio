# Library Architecture

## Overview

The compio library provides compressed archive functionality with multiple compression algorithms, block-based storage, and efficient memory management.

## Core Components

### 1. Archive Header

The archive header stores metadata including:
- Magic number (file signature)
- B-Tree root address
- File table (list of files with sizes)
- Allocator state (offset and size)
- **Compression type** - identifies which compression algorithm was used

### 2. Compression System

#### Supported Algorithms
- **ZLIB** - Default, balanced compression
- **LZ4** - Fast compression/decompression
- **Zstandard** - Modern efficient compression
- **Brotli** - High compression ratio
- **Dummy** - No compression

#### Compression Type Persistence
The compression algorithm type is stored in the archive header as `compression_type` field. When an archive is created, the selected compressor is identified and its type is saved. When reopening an archive, the library automatically:
1. Reads `compression_type` from header
2. Builds the appropriate compressor
3. Uses it for all decompression operations

This ensures data integrity regardless of the config passed when opening an existing archive.

### 3. Storage System

#### Block-based Storage
Files are split into fixed-size blocks (default 4KB). Each block is:
- Compressed independently
- Stored with metadata (original size, compression flag)
- Indexed in B-Tree by (file_hash, position)

#### Block Allocator
Manages free space in the archive file with strategies:
- **First-fit** - Use first suitable block
- **Best-fit** - Use smallest suitable block  
- **Worst-fit** - Use largest suitable block
- **Next-fit** - Continue from last position

The allocator state is persisted in the archive file for fragmentation tracking.

### 4. Indexing (B-Tree)

B-Tree index maps (file_hash, block_position) → (storage_address, block_size).
- Configurable degree (branching factor)
- Cached nodes for performance
- Persistent on disk

### 5. Caching

Three-level cache system:
- **Node cache** - B-Tree nodes
- **Block cache** - Compressed storage blocks
- **Decompression cache** - Uncompressed data

## Data Flow

### Write Operation
1. Split data into blocks
2. Compress each block with selected algorithm
3. Allocate space in archive
4. Write compressed block
5. Update B-Tree index
6. Cache decompressed data

### Read Operation
1. Query B-Tree for block addresses
2. Read compressed blocks from file
3. Decompress with stored compression type
4. Return requested data range
5. Cache for future reads

## File Format

```
[Header]
  - magic_number (4 bytes)
  - index_root (8 bytes)
  - file_size (8 bytes)
  - allocator_state_offset (8 bytes)
  - allocator_state_size (8 bytes)
  - compression_type (4 bytes)
  - files_table (variable)

[Allocator State]
  - Free block list
  - Fragmentation data

[B-Tree Nodes]
  - Index structure

[Storage Blocks]
  - Compressed data blocks
```
