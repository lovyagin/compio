# Crash Protection Overview

## Problem
In single-header archives, a crash during metadata write (flush/close) can corrupt the entire archive by leaving the header partially written or inconsistent. This makes recovery impossible or complex.

## Solution: Double Buffered Headers
We implemented a robust crash protection mechanism using two independent header slots ("Superblocks") at the beginning of the archive.

### 1. Dual Header Layout
The archive now reserves space for **two** complete header structures at the start of the file:
- **Slot A (Primary)**: Begins at offset 0.
- **Slot B (Secondary)**: Begins at offset `header.disk_size()`.

### 2. Atomic Updates via Sequence IDs
Each header includes a monotonic `sequence_id`.
- When opening an archive, both slots are read and validated.
- The valid header with the higher `sequence_id` is selected as the current state.
- During updates (flush/close), the system writes to the *inactive* slot (e.g., if A is current, write to B) with `sequence_id + 1`.
- Once the write completes successfully, the new slot becomes active.
- This ensures that at least one valid header always exists on disk, even if a crash occurs during a write.

### 3. Checksum Validation
Every header write includes a CRC32/SHA-256 checksum of its content.
- Reads verify the checksum before accepting a header.
- Corrupted headers (due to partial writes or bit rot) are rejected, falling back to the alternate slot.

### 4. Space Reservation
The `block_allocator` and `perform_defragmentation` logic have been updated to reserve space for both headers (`disk_size() * 2`) before allocating any data blocks. This prevents data overwrites when the header grows.

## Benefits
- **Atomic Metadata Updates**: No risk of corrupting the only valid header.
- **Automatic Recovery**: On next open, the library automatically detects and uses the latest valid state.
- **Corruption Detection**: Checksums prevent using garbage data.
- **Backward Compatibility**: The file format is updated (v4), but logic can be extended to support legacy reads if needed (currently enforces new format).

## Limitations: Metadata vs. Data
This protection mechanism secures the **Archive Structure** (File Table, Allocation Map, B-Tree Root). It ensures the archive remains "openable" and has a valid file list even after a crash.

However, it does **not** provide full transactional data safety (ACID) for individual file contents:
- **Data Blocks**: New or modified data blocks are written in-place or to new locations. If a crash occurs during a data write, that specific file's data may be truncated or corrupt.
- **Index Nodes**: The B-tree structure is updated in-place. A crash during an index node update could theoretically corrupt the path to a file, though the Double Buffered Header points to the last *successfully committed* root.
- **No WAL (Write-Ahead Log)**: We do not use a separate log file or journal. This means we rely on the atomic switch of the header to commit changes. Any data written *before* the header switch is "pending"; if the header switch fails (crash), that space is considered free/garbage by the old header. This is a form of "Shadow Paging" for the metadata.

## Verification
- Comprehensive tests (`test_max_files.cpp`, `test_header_validation.cpp`, `test_concurrency.cpp`) verify:
  - Recovery from corrupted slot A or B.
  - Correct selection of highest sequence ID.
  - Rejection of invalid checksums.
  - Thread-safe access to headers during concurrent operations.
- **Atomic Close**: The `compio_close_archive` operation now uses the double-buffering mechanism to ensure the final state is safely committed to disk, protecting against crashes during application shutdown.
