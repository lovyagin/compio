# Library Architecture

## Overview

The compio library provides compressed archive functionality with multiple compression algorithms, block-based storage, and efficient memory management.

## Core Components

### 1. Archive Header

The archive header stores metadata including:
- Magic number (file signature)
- B-Tree root address
- Address, size and checksum of the files table (name, size and id of every file)
- Allocator state (offset and size)
- **Compression type** - identifies which compression algorithm was used

### 2. Compression System

#### Supported Algorithms
- **ZLIB** - Balanced compression
- **LZ4** - Default, fast compression/decompression
- **Zstandard** - Modern efficient compression
- **Brotli** - High compression ratio
- **Dummy** - No compression

#### Compression Type Persistence
The compressor identifier is stored in the archive header as the `compression_type` field.
Built-in compressors use identifiers 1..5; values up to `COMPIO_COMPRESS_CUSTOM - 1` are
reserved for future built-ins, and custom compressors take identifiers from
`COMPIO_COMPRESS_CUSTOM` (0x10000) up. When an existing archive is opened, the identifier of the
supplied compressor must equal the stored one, otherwise opening fails with `EINVAL`. The library
cannot adopt the compressor from the header, because only the caller has the code of a custom
one; `compio_build_compressor_by_type` builds the matching built-in compressor.

#### Compressor per block
The header field names the compressor of *new* blocks. Every block records the codec that packed
it (`0` = stored as is, otherwise a `compression_type`), so one archive can hold blocks of several
algorithms. `compio_set_compressor` switches the algorithm for new blocks while the archive is
open. Opening an archive with a compressor other than the one in the header fails with `EINVAL`
unless `allow_compressor_change` is set in the config; then the config compressor packs the new
blocks and replaces the header field at the next publication (a read-only open leaves the header
as it is). `compio_recompress` rewrites the blocks of other codecs with the current one; a block
stored uncompressed stays as it is. Blocks of a built-in codec are decoded without any setup. A
block of an external codec needs `compio_register_compressor` after the archive is opened (a
block whose codec is unknown fails to read with `ENOTSUP`). A compressed block also carries a
checksum of its uncompressed bytes, so a compressor that decodes to something else than it
encoded is detected on read even if the stored bytes are intact.

### 3. Storage System

#### Block-based Storage
Files are split into blocks of variable size: `block_size` (default 16 KiB) is the
target used when incoming data is cut into blocks, and inserts and erases keep
every block between `block_size__minimum` and `block_size__maximum`. Each block is:
- Compressed independently
- Stored with metadata (codec, stored size, original size, owner back-reference, checksums)
- Indexed in B-Tree by (file id, position)

#### Block Allocator
Manages free space in the archive file with strategies:
- **First-fit** - Use first suitable block
- **Best-fit** - Use smallest suitable block (default)
- **Worst-fit** - Use largest suitable block
- **Next-fit** - Continue from last position

Free regions are kept in memory in two ordered indexes, by offset and by size, and are
saved in the archive on flush and close. Releasing a region and allocating with best fit
or worst fit take logarithmic time in the number of free regions; first fit and next fit
walk the regions in address order. Adjacent free regions are merged as soon as they appear.
Fragmentation is the share of the archive taken by free regions. It is checked at every
header publication (`compio_flush`, `compio_end_batch`, a checkpoint at `wal_max_size_bytes`)
and on close. Space freed since the previous publication is still referenced by the durable
header and is reused by the following writes, so a publication counts only space that stayed
free over a whole interval; close counts all free space. When the share exceeds
`fragmentation_threshold`, the archive is compacted. Compaction never writes into space
that the last published header still references: starting from the highest address, each
data block, B-tree node (copied along its path from the root), the files table and the
allocator state moves into the lowest free region below it that fits. The space it leaves
becomes free once a header without the old references is published, so compaction runs in
rounds that each end in a checkpoint, and the container is cut behind the last live byte
after the final one. A crash at any point leaves the archive as of the last checkpoint. A
gap smaller than every item above it can remain. With `fill_holes_with_zeros` freed regions are returned to the filesystem by hole punching (Linux, macOS, Windows).

### 4. Indexing (B-Tree)

B-Tree index maps (file_hash, block_position) → (storage_address, block_size).
- Configurable degree (branching factor)
- Cached nodes for performance
- Persistent on disk

### 5. Caching

Two caches, both sized in entries:
- **Node cache** (`cache_size__nodes`) - B-Tree nodes, LRU
- **Block cache** (`cache_size__blocks`) - uncompressed blocks, 2Q with a ghost queue;
  one budget shared by all files of the archive

A modified block is compressed and written when it leaves the cache or on `compio_flush`.
A flush writes what is modified and keeps both caches warm.

### 6. Crash Safety

Nothing the durable header references is overwritten before the next header is
durable: data blocks, index nodes, the files table and the allocator state are
written to fresh space, and space freed since the last publication is reused only
after the next one. A crash therefore returns the state of the last published
header (`compio_flush`, `compio_end_batch`, close, or an automatic checkpoint).

With the write-ahead log enabled, every operation outside a batch also writes
back its dirty blocks and index nodes, logs them, and then logs one record with
the resulting index root, container end and files table before its commit marker.
Automatic batches commit the same way when they end. On open, recovery replays
the committed records, adopts the last such record, rebuilds the free list from
what the index references and publishes the result, so every completed operation
survives a crash. An explicit batch is atomic: it is lost as a whole unless its
end completed.

#### Transactions
`compio_begin_transaction` publishes the pending changes and opens a transaction
over the whole archive; every operation of any thread until
`compio_commit_transaction` belongs to it, without isolation. Inside it no header
is published (`compio_flush` only writes data out), with or without the
write-ahead log, so a crash returns the state the transaction started from.
`compio_abort_transaction` returns to that state without a crash: it drops the
cached blocks and nodes with their unwritten changes, reloads the published
header, rebuilds the allocator, index and block reader from it, truncates the
journal and cuts off what the transaction appended past the container end. Open
files take their size from the restored files table. Closing the archive with a
transaction open aborts it. Compaction and recompression wait for the end of the
transaction (`EBUSY`).

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

Format v7. Two header slots of 120 bytes each start the file; the slot with
the larger valid `sequence_id` is current. All integers are little endian.

```
[Header slot] x 2
  - magic_number (4 bytes, 27110664 = format v7)
  - checksum (32 bytes, SHA-256 of the fields below)
  - sequence_id (8 bytes)
  - index_root (8 bytes)
  - file_size (8 bytes)
  - allocator_state_offset (8 bytes)
  - allocator_state_size (8 bytes)
  - compression_type (4 bytes)
  - block_size (4 bytes)
  - b_tree_degree (4 bytes)
  - files_table_addr (8 bytes, 0 when there are no files)
  - files_table_size (8 bytes, stored size)
  - files_count (8 bytes)
  - files_table_checksum (4 bytes, CRC32C of the stored bytes)
  - files_table_encoding (4 bytes, 0 = plain, otherwise the built-in compression type that packed it)

[Files Table] stored as is, or as its plain size (8 bytes) followed by the plain table packed
with the archive's compressor, when that is built-in and makes it shorter. A custom compressor
is never used, so the table is readable before any compressor is registered.
Plain form: one record per file
  - name_len (2 bytes, 1..COMPIO_FNAME_MAX_SIZE - 1)
  - size (8 bytes)
  - file_id (8 bytes)
  - name (name_len bytes, no terminator)

[Allocator State]
  - Free block list
  - Fragmentation data

[B-Tree Node]
  - signature (1 byte, 68), is_leaf (1 byte, 0 or 1), num_keys (4 bytes)
  - keys, values, then children and key additions (inner nodes only)
  - checksum (4 bytes, CRC32C of all bytes before it)

[Storage Block]
  - signature (1 byte, 175 = FNV-1a checksums, 176 = CRC32C checksums)
  - codec (4 bytes, 0 = stored as is, otherwise a compression_type)
  - size (8 bytes, stored), original_size (8 bytes)
  - owning key (16 bytes: file id and position), lets the index be rebuilt from blocks
  - checksum (4 bytes, over the owning key and the stored bytes)
  - data_checksum (4 bytes, over the uncompressed bytes; 0 for a stored block)
  - stored bytes
```
