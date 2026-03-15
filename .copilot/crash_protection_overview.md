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

## Verification
- Comprehensive tests (`test_crash_protection.cpp`, `test_header_validation.cpp`) verify:
  - Recovery from corrupted slot A or B.
  - Correct selection of highest sequence ID.
  - Rejection of invalid checksums.
  - Data integrity after multiple crash-recovery cycles.
