/**
 * @file compio.h
 * @brief Public API for the compression library.
 *
 * This header file provides the main interface for working with compressed
 * archives and files. It includes functions for opening, reading, writing,
 * and managing files within compressed archives.
 *
 * The library is designed to facilitate efficient file operations
 * in compressed environments, with support for various compression formats
 * and seamless integration into user applications.
 *
 * @par Error Handling:
 * Functions indicate success/failure via return values or errno:
 * - Functions returning int: COMPIO_SUCCESS (0) on success, COMPIO_ERROR (-1) on error
 * - Functions returning uint64_t: return value on success, 0 on error (check errno)
 * - Functions returning pointers: non-NULL on success, NULL on error (check errno)
 *
 * After an error, check errno for specific error codes:
 * - ENOENT: File/archive not found
 * - EACCES: Permission denied
 * - ENOBUFS: Buffer too small or archive full
 * - EINVAL: Invalid argument or corrupted data
 * - EIO: I/O error (disk error, corruption detected)
 * - EEXIST: File already exists (when creating)
 *
 * @par Thread Safety:
 * Archive handles must be protected during open/close operations.
 * Once initialized, concurrent reads/writes to different files are safe.
 * See individual function documentation for detailed thread-safety semantics.
 *
 * @par Transactions & Atomicity:
 * All write operations (compio_write, compio_insert, compio_erase) are backed
 * by a Write-Ahead Log (WAL) for crash resilience. A series of operations can
 * be grouped in a transaction (if using C++ API) for atomic visibility.
 *
 * @par Crash Recovery:
 * If the program crashes, a .wal file may exist alongside the archive.
 * Upon next open (compio_open_archive), WAL recovery is automatic. Uncommitted
 * changes are rolled back, ensuring consistency.
 */

#ifndef COMPIO_COMPIO_H
#define COMPIO_COMPIO_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define COMPIO_MAX_FILES 4096        /**< Default maximum number of files in archive */
#define COMPIO_MAX_FILES_LIMIT 10000000 /**< Hard upper bound accepted when reading an archive header (10M) */
#define COMPIO_FNAME_MAX_SIZE 32   /**< File name maximum length */

#define COMPIO_ERROR (-1)
#define COMPIO_SUCCESS 0

#ifdef __cplusplus
extern "C" {
#endif

#define COMPIO_MAGIC_NUMBER 27110662 /**< Bumped in format v5 (dynamic files table) */

/**
 * @brief Checksum algorithm types
 */
typedef enum {
    COMPIO_CHECKSUM_FNV1A = 0, /**< FNV-1a 32-bit (legacy default) */
    COMPIO_CHECKSUM_CRC32C = 1 /**< CRC32C (hardware accelerated where available) */
} compio_checksum_type;

/**
 * @brief Compression algorithm types
 */
typedef enum {
    /**
     * @brief Custom compression (use it, when you're using your custom compress/decompress
     * algorithm; note, that if you want to use archive, created with custom compression, in
     * different program, you'll need to provide the exact same compressor, and set compression_type
     * to custom)
     *
     */
    COMPIO_COMPRESS_CUSTOM = 0,
    COMPIO_COMPRESS_DUMMY = 1, /**< No compression (dummy) */
    COMPIO_COMPRESS_ZLIB = 2,  /**< ZLIB compression */
    COMPIO_COMPRESS_LZ4 = 3,   /**< LZ4 compression */
    COMPIO_COMPRESS_ZSTD = 4,  /**< Zstandard compression */
    COMPIO_COMPRESS_BROTLI = 5 /**< Brotli compression */
} compio_compression_type;

/**
 * @brief Compressor interface
 */
typedef struct compio_compressor {
    /**
     * @brief Compress src_size of bytes from src buffer into dst buffer.
     * On success, return 0 and write real size of compressed data into
     * dst_size. If dst buffer is too small, return non-zero code and set errno =
     * ENOBUFS.
     */
    int (*compress)(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size);

    /**
     * @brief Decompress src_size of bytes, that was previously
     * compressed with the same compressor, from src buffer into dst buffer. On
     * success, return 0 and write real size of decompressed data into dst_size.
     * If dst buffer is too small, return non-zero code and set errno = ENOBUFS.
     */
    int (*decompress)(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size);

    /**
     * @brief Get size of buffer, that needs to be provided to function compress()
     *
     */
    uint64_t (*get_bufsize)(const struct compio_compressor* comp, uint64_t src_size);

    /**
     * @brief Compression type, that will be saved in archive header (see compio_compression_type
     * for possible values)
     *
     */
    compio_compression_type compression_type;

    /**
     * @brief Compression level
     */
    int level;
} compio_compressor;

/**
 * @brief Test compressor, keeps data exactly the same
 *
 * @param result Pointer to compressor structure to initialize
 */
void compio_build_dummy_compressor(compio_compressor *result);

/**
 * @brief ZLIB compressor
 *
 * @param result Pointer to compressor structure to initialize
 */
void compio_build_zlib_compressor(compio_compressor *result);

/**
 * @brief ZLIB compressor with explicit compression level
 *
 * @param result Pointer to compressor structure to initialize
 * @param level Compression level (0 = no compression, 1 = fastest, 9 = best compression,
 *              Z_DEFAULT_COMPRESSION = default balance)
 */
void compio_build_zlib_compressor_with_level(compio_compressor *result, int level);

/**
 * @brief LZ4 compressor - very fast compression/decompression
 *
 * @param result Pointer to compressor structure to initialize
 */
void compio_build_lz4_compressor(compio_compressor *result);

/**
 * @brief LZ4 compressor with explicit acceleration level
 *
 * @param result Pointer to compressor structure to initialize
 * @param level Acceleration factor (1 = default speed/ratio balance, higher = faster but lower
 *              compression ratio)
 */
void compio_build_lz4_compressor_with_level(compio_compressor *result, int level);

/**
 * @brief Zstandard (zstd) compressor - modern efficient compression
 *
 * @param result Pointer to compressor structure to initialize
 */
void compio_build_zstd_compressor(compio_compressor *result);

/**
 * @brief Zstandard (zstd) compressor with explicit compression level
 *
 * @param result Pointer to compressor structure to initialize
 * @param level Compression level (1 = fastest, 22 = best compression, default = 5)
 */
void compio_build_zstd_compressor_with_level(compio_compressor *result, int level);

/**
 * @brief Brotli compressor - high compression ratio
 *
 * @param result Pointer to compressor structure to initialize
 */
void compio_build_brotli_compressor(compio_compressor *result);

/**
 * @brief Brotli compressor with explicit compression level
 *
 * @param result Pointer to compressor structure to initialize
 * @param level Compression level (0 = fastest, 11 = best compression, default = 5)
 */
void compio_build_brotli_compressor_with_level(compio_compressor *result, int level);

/**
 * @brief Build compressor based on compression type
 *
 * This is a helper function that automatically initializes the appropriate
 * compressor based on the compression type. Useful for auto-detecting
 * compression type from archive headers.
 *
 * @param result Pointer to compressor structure to initialize
 * @param type Compression type to use
 */
void compio_build_compressor_by_type(compio_compressor *result, compio_compression_type type);

/**
 * @brief Memory allocation strategies for managing free blocks in archive
 */
typedef enum {
    COMPIO_ALLOC_FIRST_FIT, /**< First-fit allocation strategy */
    COMPIO_ALLOC_BEST_FIT,  /**< Best-fit allocation strategy */
    COMPIO_ALLOC_WORST_FIT, /**< Worst-fit allocation strategy */
    COMPIO_ALLOC_NEXT_FIT   /**< Next-fit allocation strategy */
} compio_allocation_strategy;

/**
 * @brief WAL synchronization mode
 */
typedef enum {
    COMPIO_WAL_SYNC_ALWAYS = 0,    /**< fsync after every transaction (default, strict durability) */
    COMPIO_WAL_SYNC_NORMAL = 1,    /**< write to OS buffer, fsync only on checkpoint/close/flush */
    COMPIO_WAL_SYNC_OFF = 2        /**< do not fsync WAL on commit; checkpoint/close/flush may still fsync (dangerous, for testing/temp files) */
} compio_wal_sync_mode;

/**
 * @brief Configuration structure for archive creation
 *
 * Contains all settings for archive behavior including compression,
 * indexing, caching, and memory allocation strategies.
 */
typedef struct {
    compio_compressor compressor; /**< Compressor for data blocks */

    int b_tree_degree;       /**< B-Tree branching factor (typically 3-10). Set to 0 to auto-detect on open. */
    int block_size;          /**< Block size for splitting files (in bytes). Set to 0 to auto-detect on open. */
    int block_size__minimum; /** Minimum size of an uncompressed block */
    int block_size__maximum; /** Maximum size of an uncompressed block */

    int cache_size__nodes;  /**< Maximum B-tree nodes kept in memory */
    int cache_size__blocks; /**< Maximum storage blocks kept in memory */

    compio_allocation_strategy allocation_strategy; /**< Free block selection strategy */
    bool fill_holes_with_zeros;      /**< Zero-fill freed blocks for sparse file optimization */
    uint8_t fragmentation_threshold; /**< Trigger defragmentation when fragmentation exceeds this
                                        percentage (1-100) */
    int max_files; /**< Initial capacity for the files table (default: COMPIO_MAX_FILES).
                        The table grows dynamically, so this is not a hard limit.
                        For archives using the v5 dynamic files table, this capacity may grow
                        beyond the initial value (subject to the hard upper bound COMPIO_MAX_FILES_LIMIT).
                        For legacy/fixed-table formats, the on-disk files table size is fixed, so this
                        may act as an effective limit. Set to 0 to use the default capacity. */
    uint64_t wal_max_size_bytes; /**< Maximum size of WAL file in bytes before forcing a checkpoint. 0 = unlimited. */
    compio_checksum_type checksum_type; /**< Checksum algorithm for data blocks */
    compio_wal_sync_mode wal_sync_mode; /**< WAL synchronization mode */
    int auto_batch_size; /**< Number of sequential operations to auto-batch (0 = disabled, default: 8) */
    bool enable_wal; /**< Maintain the write-ahead log (default: true). When false, writes go
                          straight to the archive with no journaling: maximum write throughput, but
                          NO crash safety and NO recovery on reopen. Intended for scratch/throwaway
                          archives or benchmarking the cost of journaling itself. */
} compio_config;

/**
 * @brief Default configuration
 *
 * @param result Pointer to config structure to initialize with defaults
 */
void compio_build_default_config(compio_config *result);

/**
 * @brief Get compression type from header of existing archive
 *
 * @param fp Path to archive file
 * @param t Pointer to compression_type to receive the result
 * @return COMPIO_SUCCESS on success, COMPIO_ERROR on failure
 */
int compio_get_compression_type(const char *fp, compio_compression_type *t);

/**
 * @brief Opened archive
 */
typedef struct compio_archive compio_archive;

/**
 * @brief Opened file inside of an archive (opaque pointer)
 *
 * Represents an open file handle for reading/writing within a compio_archive.
 * See compio_open_file() to open a file.
 *
 * @par Thread Safety:
 * A file handle cannot be safely used by multiple threads simultaneously.
 * Each thread should have its own file handle (via compio_open_file).
 * However, multiple threads CAN read from or write to DIFFERENT files
 * within the same archive simultaneously.
 */
typedef struct compio_file compio_file;

/**
 * @brief Opens an archive for reading or writing
 *
 * Opens an existing archive or creates a new one. Supports modes similar to fopen().
 * Initializes internal structures including B-tree index, file table, and WAL.
 *
 * @param[in] fp Archive file path (filesystem path)
 * @param[in] mode File open mode. Supported modes:
 *   - "r":   Read-only (archive must exist)
 *   - "r+":  Read-write (archive must exist)
 *   - "w":   Create new (truncate if exists)
 *   - "w+":  Create new (truncate if exists)
 *   - "a":   Append (create if not exists)
 *   - "a+":  Append (create if not exists)
 * @param[in] c Configuration pointer (see compio_config). If NULL, defaults are used.
 *            For auto-detection of block_size and b_tree_degree, set to 0 (Smart Open).
 * @return Pointer to compio_archive on success, NULL on error (check errno)
 *
 * @par Thread Safety:
 * The returned archive handle should be protected by external synchronization
 * when calling compio_open_archive/compio_close_archive. However, once files
 * are opened, concurrent operations are safe:
 * - Multiple threads can read different files simultaneously
 * - Multiple threads can write different files simultaneously
 * - A single file cannot be read/written by multiple threads concurrently
 *
 * @par WAL Recovery:
 * If a .wal file exists alongside the archive, recovery is attempted automatically.
 * Uncommitted transactions are rolled back, ensuring data consistency.
 *
 * @par Smart Open (auto-detection):
 * When config->block_size or config->b_tree_degree are 0, they are auto-detected
 * from the archive header. Useful when opening archives with non-default parameters.
 *
 * @see compio_close_archive()
 * @see compio_open_file()
 * @see compio_config
 */
compio_archive *compio_open_archive(const char *fp, const char *mode, const compio_config *c);

/**
 * @brief Opens a file inside an archive
 *
 * Opens or creates a file with the given name within the archive.
 * Returns a file handle for subsequent read/write operations.
 *
 * @param[in] name Internal filename (max COMPIO_FNAME_MAX_SIZE characters)
 * @param[in] archive Opened archive handle
 * @return Pointer to compio_file on success, NULL on error
 *
 * @par Thread Safety:
 * Multiple threads can call compio_open_file on the same archive simultaneously,
 * but each returned file handle must be used by only one thread.
 * If multiple threads need to access the same filename, each must call
 * compio_open_file separately to get their own handle.
 *
 * @see compio_close_file()
 */
compio_file *compio_open_file(const char *name, compio_archive *archive);

/**
 * @brief Writes data to a file
 *
 * Appends data at current file position. File position advances by the number
 * of bytes successfully written.
 *
 * @param[in] ptr Pointer to data to write. Must not be NULL if size > 0.
 * @param[in] size Number of bytes to write
 * @param[in,out] file Opened file handle (file position updated)
 * @return Number of bytes successfully written. May be less than size on error.
 *         Returns 0 on error; check errno for details.
 *
 * @par Performance:
 * O(log N) in archive size (B-tree lookup). Sequential writes are optimized via
 * internal caching, so multiple sequential writes are faster than random writes.
 *
 * @par Thread Safety:
 * NOT thread-safe. Do not call compio_write on the same file handle from
 * multiple threads concurrently. Each thread must have its own file handle.
 *
 * @par Atomicity:
 * Partial writes are possible if the archive is full or disk error occurs.
 * Caller should check return value and retry if needed.
 *
 * @see compio_read(), compio_seek()
 */
uint64_t compio_write(const void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Reads data from a file
 *
 * Reads up to `size` bytes from current file position. File position advances
 * by the number of bytes successfully read.
 *
 * @param[out] ptr Pointer to buffer to receive data. Must not be NULL if size > 0.
 * @param[in] size Number of bytes to read
 * @param[in,out] file Opened file handle (file position updated)
 * @return Number of bytes successfully read. Less than size if EOF reached or error.
 *         Returns 0 on error or EOF; check errno or compio_tell() to distinguish.
 *
 * @par Performance:
 * O(log N) in archive size (B-tree lookup). Sequential reads are highly optimized
 * via leaf caching, providing near-sequential disk I/O performance.
 *
 * @par Thread Safety:
 * Multiple threads CAN call compio_read on different file handles (different files)
 * concurrently. However, compio_read on the SAME file handle is NOT thread-safe.
 *
 * @par Consistency:
 * Reads see all committed writes (from compio_write). Uncommitted data is not visible.
 *
 * @see compio_write(), compio_seek()
 */
uint64_t compio_read(void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Inserts data into file, shifting existing data forward
 *
 * Inserts `size` bytes at current file position, shifting all data after the
 * insertion point forward. File size increases by `size`. File position advances
 * past the inserted data.
 *
 * @param[in] ptr Pointer to data to insert. Must not be NULL if size > 0.
 * @param[in] size Number of bytes to insert
 * @param[in,out] file Opened file handle (file position updated)
 * @return Number of bytes successfully inserted. May be less than size on error.
 *         Returns 0 on error; check errno.
 *
 * @par Performance:
 * O(N log N) where N is the number of blocks after insertion point.
 * Expensive operation - avoid if possible; consider append (compio_write) instead.
 *
 * @par Thread Safety:
 * NOT thread-safe. Do not call on same file handle from multiple threads.
 *
 * @par Atomicity:
 * Insertion is atomic at the transaction level (WAL-backed).
 *
 * @see compio_erase(), compio_write()
 */
uint64_t compio_insert(const void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Erases data from file, shifting remaining data backward
 *
 * Removes `size` bytes at current file position, shifting all data after the
 * erased region backward. File size decreases by `size`. File position remains
 * at the start of the erased region (pointing to next data).
 *
 * @param[in] size Number of bytes to erase
 * @param[in,out] file Opened file handle (file position updated)
 * @return Number of bytes successfully erased. May be less than size if EOF or error.
 *         Returns 0 on error; check errno.
 *
 * @par Performance:
 * O(N log N) where N is the number of blocks after erased region.
 * Expensive operation - avoid if possible.
 *
 * @par Thread Safety:
 * NOT thread-safe. Do not call on same file handle from multiple threads.
 *
 * @par Atomicity:
 * Erasure is atomic at the transaction level (WAL-backed).
 *
 * @see compio_insert(), compio_seek()
 */
uint64_t compio_erase(uint64_t size, compio_file *file);

typedef enum { COMPIO_SEEK_SET, COMPIO_SEEK_CUR, COMPIO_SEEK_END } compio_seek_mode;

/**
 * @brief Sets file position
 *
 * Moves the file pointer to a new position for subsequent read/write operations.
 *
 * @param[in,out] file Opened file handle
 * @param[in] offset Offset in bytes (can be negative)
 * @param[in] origin Reference point for offset:
 *   - COMPIO_SEEK_SET (0): Offset from beginning of file
 *   - COMPIO_SEEK_CUR (1): Offset from current position
 *   - COMPIO_SEEK_END (2): Offset from end of file (negative for before EOF)
 * @return COMPIO_SUCCESS (0) on success
 * @retval COMPIO_ERROR (-1) on error (invalid origin, seeking before BOF, etc.)
 *         Check errno for details
 *
 * @par Behavior:
 * - Seeking past EOF is allowed (next write will extend file)
 * - Seeking before BOF fails with EINVAL
 *
 * @see compio_tell()
 */
int compio_seek(compio_file *file, int64_t offset, uint8_t origin);

/**
 * @brief Gets current file position
 *
 * @param[in] file Opened file handle
 * @return Current position in bytes (offset from start of file)
 *
 * @note Always succeeds (cannot fail)
 *
 * @see compio_seek()
 */
uint64_t compio_tell(compio_file *file);

/**
 * @brief Gets total file size
 *
 * @param[in] file Opened file handle
 * @return File size in bytes (0 if empty or error)
 *
 * @note Always succeeds
 *
 * @see compio_get_size()
 */
uint64_t compio_get_size(compio_file *file);

/**
 * @brief Flushes all cached data to disk
 *
 * Forces all buffered changes to be written to the underlying archive file and
 * synced to the filesystem. Ensures durability of all prior operations.
 *
 * @param[in] archive Opened archive handle
 *
 * @par Thread Safety:
 * Can be called concurrently with read/write operations, but should not be
 * called concurrently by multiple threads (only one flush at a time).
 *
 * @par Performance:
 * Expensive operation (involves fsync). Use sparingly in performance-critical code.
 *
 * @note Does NOT close files or archive - use compio_close_file/compio_close_archive for that
 *
 * @see compio_close_archive()
 */
void compio_flush(compio_archive *archive);

/**
 * @brief Removes a file from the archive
 *
 * Permanently removes the file with the given name and reclaims its storage space.
 * File must not be open (close with compio_close_file first).
 *
 * @param[in] archive Opened archive handle
 * @param[in] name Internal filename to remove
 * @return COMPIO_SUCCESS (0) on success
 * @retval COMPIO_ERROR (-1) on error
 *
 * @par Error Codes (check errno):
 * - ENOENT: File does not exist
 * - EACCES: File is currently open or permission denied
 * - EIO: I/O error
 *
 * @par Thread Safety:
 * Do not remove a file that is currently open in another thread.
 *
 * @par Performance:
 * O(1) average case (swap-remove strategy). Fast operation.
 *
 * @see compio_open_file()
 */
int compio_remove_file(compio_archive *archive, const char *name);

/**
 * @brief Closes an opened file
 *
 * Closes the file and frees associated resources. File data is not lost
 * (use compio_remove_file to delete the file from archive).
 *
 * @param[in] file Opened file handle
 * @return COMPIO_SUCCESS (0) on success
 * @retval COMPIO_ERROR (-1) on error
 *
 * @par Thread Safety:
 * Do not close a file from one thread while another thread is reading/writing it.
 *
 * @note After close, the file handle is invalid and must not be reused
 *
 * @see compio_open_file()
 */
int compio_close_file(compio_file *file);

/**
 * @brief Closes the archive
 *
 * Closes the archive, flushing all buffered data and syncing metadata to disk.
 * All open files should be closed before calling this (or close them implicitly).
 *
 * @param[in] archive Opened archive handle
 * @return COMPIO_SUCCESS (0) on success
 * @retval COMPIO_ERROR (-1) on error (check errno)
 *
 * @par Thread Safety:
 * Do not call while other threads are performing read/write operations.
 * Must serialize access to the archive handle.
 *
 * @par Behavior:
 * - Flushes all cached data to disk
 * - Syncs WAL checkpoint (if applicable)
 * - Closes the archive file
 *
 * @note After close, the archive handle is invalid and must not be reused
 * @note Any WAL file is preserved for potential recovery on next open
 *
 * @see compio_open_archive()
 */
int compio_close_archive(compio_archive *archive);

/**
 * @brief Statistics about allocator fragmentation
 */
typedef struct compio_fragmentation_stats {
    size_t num_free_regions;      /**< Number of separate free regions */
    size_t total_free_bytes;       /**< Total free space in bytes */
    size_t largest_free_region;    /**< Size of largest contiguous free block */
    size_t smallest_free_region;   /**< Size of smallest free block */
    double avg_free_region_size;   /**< Average size of free regions */
    uint8_t fragmentation_percent; /**< Overall fragmentation percentage (0-100) */
} compio_fragmentation_stats;

/**
 * @brief Get detailed fragmentation statistics from archive allocator
 *
 * This function provides insight into the internal state of the block allocator,
 * showing how fragmented the free space is. High fragmentation (many small free
 * regions) can impact allocation performance and space efficiency.
 *
 * @param archive opened archive
 * @param stats pointer to structure to fill with statistics
 * @return COMPIO_SUCCESS on success, COMPIO_ERROR on failure
 */
int compio_get_fragmentation_stats(compio_archive *archive, compio_fragmentation_stats *stats);

/**
 * @brief Defragment the archive, compacting data blocks and reclaiming free space.
 *
 * Runs the allocator maintenance pass immediately (regardless of the configured
 * fragmentation threshold).  The archive must be opened in write mode.
 *
 * @param archive opened archive (write mode)
 * @return COMPIO_SUCCESS on success, COMPIO_ERROR on failure
 */
int compio_defragment(compio_archive *archive);

/**
 * @brief Begin a batch of write operations
 *
 * Starts a batch transaction that defers WAL commits until compio_end_batch().
 * All write/insert/erase operations between begin and end are logged to WAL
 * but durability depends on the configured WAL sync mode at end_batch().
 * Batches can be nested; only the outermost end_batch triggers commit.
 *
 * @param archive opened archive (write mode)
 * @return COMPIO_SUCCESS on success, COMPIO_ERROR on failure
 */
int compio_begin_batch(compio_archive *archive);

/**
 * @brief End a batch of write operations
 *
 * Commits all buffered WAL operations. Durability behavior depends on
 * the configured wal_sync_mode: ALWAYS performs fsync, NORMAL only fflush,
 * OFF skips both. If this is the outermost batch, performs the commit.
 * 
 * @param archive opened archive (write mode)
 * @return COMPIO_SUCCESS on success, COMPIO_ERROR on failure
 */
int compio_end_batch(compio_archive *archive);

/**
 * @brief Repairs/recovers data from a corrupted archive
 *
 * Scans the archive file for valid storage blocks and attempts to reconstruct
 * files. Performs intensive disk I/O scanning for valid block signatures.
 * Recovered files are written to the output directory with sanitized names.
 *
 * @param[in] path Path to the corrupted archive file
 * @param[in] output_dir Directory where recovered files will be written
 *            Must exist and be writable. Created if it doesn't exist.
 * @return Number of successfully recovered files (>= 0) on success
 * @retval COMPIO_ERROR (-1) on fatal error
 *
 * @par Recovery Behavior:
 * - Scans entire archive for block signatures (BLOCK, INDEX_NODE, HEADER records)
 * - If archive header is readable, uses it to restore original filenames
 * - If B-tree index is readable, uses it to properly order blocks
 * - Falls back to sequential ordering if index is corrupted
 * - Skips blocks with invalid checksums (detected via CRC32C/FNV1A)
 * - Creates recovered files as: recovered_0.dat, recovered_1.dat, etc.
 *
 * @par Error Codes (check errno):
 * - ENOENT: Archive file not found or output directory cannot be created
 * - EACCES: Permission denied reading archive or writing to output dir
 * - EIO: I/O error reading corrupted archive
 * - ENOMEM: Out of memory during recovery
 * - ENOBUFS: Output directory full
 *
 * @par Thread Safety:
 * Archive must NOT be open concurrently. Only one recovery at a time.
 *
 * @par Performance:
 * Expensive operation - scans entire archive sequentially. Time proportional
 * to archive size. Recovery is best-effort and may not recover all data.
 *
 * @par Limitations:
 * - Inline metadata (not in blocks) is lost
 * - File structure (directory hierarchies) is not recovered
 * - Encryption/compression settings are recovered from header if readable
 * - Small files entirely in header may not be recoverable
 *
 * @see compio_open_archive()
 */
int compio_repair(const char *path, const char *output_dir);

/**
 * @brief Check if auto-batching is currently active for a file
 *
 * @param file File handle
 * @return 1 if auto-batching is active, 0 if not
 */
int compio_is_auto_batching(compio_file *file);

/**
 * @brief Get the current operation counter used for auto-batching for a file
 *
 * This returns the logical counter of write-like operations that the
 * auto-batching logic uses to decide when to start, extend, or end batches.
 * The counter may be non-zero even when auto-batching is not currently
 * active (see compio_is_auto_batching()), for example for pre-batch
 * sequential operations or when auto-batching is disabled.
 *
 * @param file File handle
 * @return Current logical operation counter used by the auto-batching logic
 */
int compio_get_auto_batch_count(compio_file *file);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_COMPIO_H
