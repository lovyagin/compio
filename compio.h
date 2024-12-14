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

/**
 * @brief Compressor interface
 */
typedef struct compio_compressor {
    /**
     * @brief Compress src_size of bytes from src buffer into dst buffer.
     * On success, return 0 and write real size of compressed data into
     * dst_size. If dst buffer is to small, return non-zero code and set errno =
     * ENOBUFS.
     */
    int (*compress)(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size);

    /**
     * @brief Decompress src_size of bytes, that was previously
     * compressed with the same compressor, from src buffer into dst buffer. On
     * success, return 0 and write real size of decompressed data into dst_size.
     * If dst buffer is to small, return non-zero code and set errno = ENOBUFS.
     */
    int (*decompress)(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size);
} compio_compressor;

/**
 * @brief Test compressor, keeps data exactly the same
 *
 * @param result
 */
void compio_build_dummy_compressor(compio_compressor* result);

/**
 * @brief Configuration
 */
typedef struct {
    /**
     * @brief Compressor, that will be used for this file
     */
    compio_compressor compressor;

    int b_tree_degree; /**< Maximum number of children of B-Tree node */
    int block_size;

    /**
     * @brief Fill deleted blocks with zeros, so that OS may optimize it (see
     * sparse files)
     */
    bool fill_holes_with_zeros;
} compio_config;

/**
 * @brief Default configuration
 *
 * @param result
 */
void compio_build_default_config(compio_config* result);

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
compio_archive* compio_open_archive(const char* fp, const char* mode, const compio_config* c);

/**
 * @brief Open file inside of an opened archive
 *
 * @param name internal filename
 * @param archive opened archive
 * @return compio_file*
 */
compio_file* compio_open_file(const char* name, compio_archive* archive);

/**
 * @brief Write block of data to file
 *
 * @param ptr pointer to data
 * @param size size in bytes
 * @param file opened file
 * @return uint64_t
 */
uint64_t compio_write(const void* ptr, uint64_t size, compio_file* file);

/**
 * @brief Read block of data from file
 *
 * @param ptr pointer to buffer
 * @param size size in bytes
 * @param file opened file
 * @return uint64_t
 */
uint64_t compio_read(void* ptr, uint64_t size, compio_file* file);

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
int compio_seek(compio_file* file, uint64_t offset, uint8_t origin);

/**
 * @brief Get current position inside of a file
 *
 * @param file opened file
 * @return long
 */
uint64_t compio_tell(compio_file* file);

/**
 * @brief Remove file from archive
 *
 * @param archive opened archive
 * @param name internal filename
 * @return int
 */
int compio_remove_file(compio_archive* archive, const char* name);

/**
 * @brief Close opened file
 *
 * @param file opened file
 * @return int
 */
int compio_close_file(compio_file* file);

/**
 * @brief Close opened file
 *
 * @param archive opened archive
 * @return int
 */
int compio_close_archive(compio_archive* archive);

#endif // COMPIO_COMPIO_H
