# Compio Multithreading Architecture

This document describes the thread-safety mechanisms implemented in the Compio library, enabling concurrent read/write operations.

## Core Concepts

Compio now supports the Reader-Writer Lock pattern using C++17 `std::shared_mutex` and `std::atomic`. This allows multiple concurrent readers while ensuring exclusive access for writers, maximizing throughput for read-heavy workloads.

### Synchronization Primitives

1.  **`std::shared_mutex` in `compio_archive`**:
    -   Protects the archive structure and file table.
    -   **Readers (`compio_read`, `compio_open_file`)**: Acquire `std::shared_lock` (shared ownership). Multiple threads can read simultaneously.
    -   **Writers (`compio_write`, `compio_insert`, `compio_erase`)**: Acquire `std::unique_lock` (exclusive ownership). Blocks all other readers and writers.
    -   **Management (`compio_defragment`, `compio_close`)**: Acquire `std::unique_lock` to ensure safe modification/closure.

2.  **`io_mutex` (std::mutex)**:
    -   Protects low-level file I/O operations (`fseek`, `fread`, `fwrite`).
    -   Ensures that file pointer positioning and data transfer are atomic operations, preventing data corruption from interleaved I/O.
    -   Passed down to `storage_block_reader` and `block_allocator`.

3.  **Atomic Reference Counting**:
    -   `smart_infile_object` uses `std::atomic<int>` for reference counting.
    -   Destruction logic uses `fetch_sub(1) == 1` to ensure thread-safe cleanup without locks on the hot path.

### Component Thread Safety

-   **`lru_cache`**: Protected by internal mutexes to allow safe concurrent access from multiple threads (e.g., during `read_block`).
-   **`block_allocator`**: Protected by `io_mutex` when modifying free lists or file size.
-   **`temporary_index`**: Protected by `temp_index_mutex` in `storage_block_reader` to handle updates from block destructors safely.
-   **`btree`**: Thread-safe due to exclusive locking at the archive level (writers lock the whole archive). Concurrent readers are safe as they don't modify the tree structure.

### Key Workflows

-   **Reading**: `compio_read` locks `archive->mutex` (shared). It accesses `files_table`, opens/finds the file, and reads data using `io_mutex` for disk access. Multiple `compio_read` calls can proceed in parallel, serializing only at the disk I/O level.
-   **Writing**: `compio_write` locks `archive->mutex` (exclusive). It modifies the file, allocates blocks, updates the B-tree index, and writes to disk. No other operations can occur on the archive during this time.
-   **File Opening**: `compio_open_file` uses shared lock to find the file. If not found (and mode implies creation), it upgrades to exclusive lock (handled by `compio_create_file` logic, though currently `compio_open_file` is read-only regarding file creation if not using `COMPIO_OPEN_WRITE`).

### Performance Considerations

-   **Reader Scalability**: Readers scale well as they only contend for `io_mutex` during actual I/O, not during processing/decompression.
-   **Writer Exclusion**: Writers block everything, ensuring consistency but limiting concurrency during heavy write bursts.
-   **Lock Granularity**: Coarse-grained locking at archive level simplifies correctness for complex B-tree operations while `io_mutex` handles fine-grained I/O protection.

This architecture ensures data integrity and prevents race conditions while providing performance benefits for multi-threaded applications.
