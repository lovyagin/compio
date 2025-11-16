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

#define COMPIO_MAX_FILES 64      /**< Maximum number of files in archive */
#define COMPIO_FNAME_MAX_SIZE 32 /**< File name maximum length */

#define COMPIO_ERROR (-1)
#define COMPIO_SUCCESS 0

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Configuration structure for archive creation
 *
 * Contains all settings for archive behavior including compression,
 * indexing, caching, and memory allocation strategies.
 */
typedef struct {
    compio_compressor compressor; /**< Compressor for data blocks */

    int b_tree_degree; /**< B-Tree branching factor (typically 3-10) */
    int block_size;    /**< Block size for splitting files (in bytes) */

    int cache_size__nodes;  /**< Maximum B-tree nodes kept in memory */
    int cache_size__blocks; /**< Maximum storage blocks kept in memory */

    compio_allocation_strategy allocation_strategy; /**< Free block selection strategy */
    bool fill_holes_with_zeros;      /**< Zero-fill freed blocks for sparse file optimization */
    uint8_t fragmentation_threshold; /**< Trigger defragmentation when fragmentation exceeds this
                                        percentage (1-100) */
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
 * @return uint64_t
 */
uint64_t compio_write(const void *ptr, uint64_t size, compio_file *file);

/**
 * @brief Read block of data from file
 *
 * @param ptr pointer to buffer
 * @param size size in bytes
 * @param file opened file
 * @return uint64_t
 */
uint64_t compio_read(void *ptr, uint64_t size, compio_file *file);

#define COMP_SEEK_SET 0
#define COMP_SEEK_CUR 1
#define COMP_SEEK_END 2

/**
 * @brief Set current position inside of a file
 *
 * @param file opened file
 * @param offset offset in bytes
 * @param origin position, used as reference for the offset
 * `origin` possible values:
 *  - COMP_SEEK_SET - offset is counted from the beginning of a file
 *  - COMP_SEEK_CUR - offset is counter from current position
 *  - COMP_SEEK_END - offset is counter from the end of a file
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

#ifdef __cplusplus
}
#endif

#endif // COMPIO_COMPIO_H
