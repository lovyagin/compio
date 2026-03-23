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
    int (*compress)(void *dst, uint64_t *dst_size, const void *src, uint64_t src_size);

    /**
     * @brief Decompress src_size of bytes, that was previously
     * compressed with the same compressor, from src buffer into dst buffer. On
     * success, return 0 and write real size of decompressed data into dst_size.
     * If dst buffer is too small, return non-zero code and set errno = ENOBUFS.
     */
    int (*decompress)(void *dst, uint64_t *dst_size, const void *src, uint64_t src_size);

    /**
     * @brief Get size of buffer, that needs to be provided to function compress()
     *
     */
    uint64_t (*get_bufsize)(uint64_t src_size);

    /**
     * @brief Compression type, that will be saved in archive header (see compio_compression_type
     * for possible values)
     *
     */
    compio_compression_type compression_type;
} compio_compressor;

/**
 * @brief Test compressor, keeps data exactly the same
 *
 * @param result
 */
void compio_build_dummy_compressor(compio_compressor *result);

/**
 * @brief ZLIB compressor
 *
 * @param result
 */
void compio_build_zlib_compressor(compio_compressor *result);

/**
 * @brief LZ4 compressor - very fast compression/decompression
 *
 * @param result
 */
void compio_build_lz4_compressor(compio_compressor *result);

/**
 * @brief Zstandard (zstd) compressor - modern efficient compression
 *
 * @param result
 */
void compio_build_zstd_compressor(compio_compressor *result);

/**
 * @brief Brotli compressor - high compression ratio
 *
 * @param result
 */
void compio_build_brotli_compressor(compio_compressor *result);

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
    int max_files; /**< Maximum number of files in a single archive (default: COMPIO_MAX_FILES).
360.                         Set to 0 to use the default limit (COMPIO_MAX_FILES) when creating a new archive,
361.                         or to read the limit from the archive header when opening an existing one. */
    uint64_t wal_max_size_bytes; /**< Maximum size of WAL file in bytes before forcing a checkpoint. 0 = unlimited. */
    compio_checksum_type checksum_type; /**< Checksum algorithm for data blocks */
    compio_wal_sync_mode wal_sync_mode; /**< WAL synchronization mode */
} compio_config;

/**
 * @brief Default configuration
 *
 * @param result
 */
void compio_build_default_config(compio_config *result);

/**
 * @brief Get compression type from header of existing archive
 *
 * @param fp path to archive file
 * @param t pointer t compression_type object
 * @return int
 */
int compio_get_compression_type(const char *fp, compio_compression_type *t);

/**
 * @brief Opened archive
 */
typedef struct compio_archive compio_archive;

/**
 * @brief Opened file inshide of an archive
 */
typedef struct compio_file compio_file;

/**
 * @brief Open archive
 *
 * @param fp path to archive file
 * @param mode mode (https://en.cppreference.com/w/cpp/io/c/fopen)
 * @param c configuration
 * @return compio_archive*
 */
compio_archive *compio_open_archive(const char *fp, const char *mode, const compio_config *c);

/**
 * @brief Open file inside of an opened archive
 *
 * @param name internal filename
 * @param archive opened archive
 * @return compio_file*
 */
compio_file *compio_open_file(const char *name, compio_archive *archive);

/**
 * @brief Write block of data to file
 *
 * @param ptr pointer to data
 * @param size size in bytes
 * @param file opened file
 * @return uint64_t number of successfully written bytes
 */
uint64_t compio_write(const void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Read block of data from file
 *
 * @param ptr pointer to buffer
 * @param size size in bytes
 * @param file opened file
 * @return uint64_t number of successfully read bytes
 */
uint64_t compio_read(void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Insert block of data into file at current position, shifting existing data
 *
 * @param ptr pointer to data to insert
 * @param size size of data to insert in bytes
 * @param file opened file
 * @return uint64_t number of successfully inserted bytes
 */
uint64_t compio_insert(const void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Erase block of data from file at current position, shifting remaining data
 *
 * @param size number of bytes to erase
 * @param file opened file handle
 * @return uint64_t number of successfully erased bytes
 */
uint64_t compio_erase(uint64_t size, compio_file *file);

typedef enum { COMPIO_SEEK_SET, COMPIO_SEEK_CUR, COMPIO_SEEK_END } compio_seek_mode;

/**
 * @brief Set current position inside of a file
 *
 * @param file opened file
 * @param offset offset in bytes
 * @param origin position, used as reference for the offset
 * `origin` possible values:
 *  - COMPIO_SEEK_SET - offset is counted from the beginning of a file
 *  - COMPIO_SEEK_CUR - offset is counter from current position
 *  - COMPIO_SEEK_END - offset is counter from the end of a file
 * @return int
 */
int compio_seek(compio_file *file, int64_t offset, uint8_t origin);

/**
 * @brief Get current position inside of a file
 *
 * @param file opened file
 * @return long
 */
uint64_t compio_tell(compio_file *file);

/**
 * @brief Get file size
 *
 * @param file opened file
 * @return long
 */
uint64_t compio_get_size(compio_file *file);

/**
 * @brief Flush all cached data to filesystem
 *
 * @param archive opened archive
 */
void compio_flush(compio_archive *archive);

/**
 * @brief Remove file from archive
 *
 * @param archive opened archive
 * @param name internal filename
 * @return int
 */
int compio_remove_file(compio_archive *archive, const char *name);

/**
 * @brief Close opened file
 *
 * @param file opened file
 * @return int
 */
int compio_close_file(compio_file *file);

/**
 * @brief Close opened file
 *
 * @param archive opened archive
 * @return int
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
 * @brief Repair/Recover data from a corrupted archive
 *
 * Scans the archive file for valid storage blocks and attempts to reconstruct
 * files. If header is available, uses it to restore filenames.
 * If index is available, uses it to order blocks.
 *
 * @param path Path to the corrupted archive
 * @param output_dir Directory to dump recovered files
 * @return Number of recovered files (>= 0) on success, COMPIO_ERROR on failure
 */
int compio_repair(const char *path, const char *output_dir);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_COMPIO_H
