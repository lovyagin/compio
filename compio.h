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

#include <stdio.h>

/**
 * @struct compio_archive
 * @brief Represents a compressed archive.
 */
typedef struct {
    int mode; // 1 << 0 - read, 1 << 1 - write
    // ...
} compio_archive;

/**
 * @struct compio_file
 * @brief Represents a file within a compressed archive.
 */
typedef struct {
    int mode; // 1 << 0 - read, 1 << 1 - write
    size_t cursor; // Current position within the file
    // ...
} compio_file;

/**
 * @typedef compio_errcode
 * @brief Represents an error code for operations.
 */
typedef int compio_errcode;

/**
 * @brief Opens a compressed archive.
 *
 * @param fp Path to the archive file.
 * @param mode File mode ("r" for read, "w" for write).
 * @param error Pointer to store error code if the operation fails.
 * @return Pointer to the opened archive, or NULL if an error occurs.
 */
compio_archive* compio_open_archive(const char* fp, const char* mode, compio_errcode* error);

/**
 * @brief Opens a file within a compressed archive.
 *
 * @param a Pointer to the opened archive.
 * @param fp Path to the file within the archive.
 * @param mode File mode ("r" for read, "w" for write).
 * @param error Pointer to store error code if the operation fails.
 * @return Pointer to the opened file, or NULL if an error occurs.
 */
compio_file* compio_open_file(compio_archive* a, const char* fp, const char* mode, compio_errcode* error);

/**
 * @brief Writes data to a file in the archive.
 *
 * @param ptr Pointer to the data to be written.
 * @param size Size of each data element in bytes.
 * @param count Number of elements to write.
 * @param f Pointer to the file to write to.
 * @return The number of elements successfully written.
 */
size_t compio_write(const void* ptr, size_t size, size_t count, compio_file* f);

/**
 * @brief Reads data from a file in the archive.
 *
 * @param ptr Pointer to the buffer where data will be read into.
 * @param size Size of each data element in bytes.
 * @param count Number of elements to read.
 * @param f Pointer to the file to read from.
 * @return The number of elements successfully read.
 */
size_t compio_read(void* ptr, size_t size, size_t count, compio_file* f);

/**
 * @brief Moves the file pointer to a specific location.
 *
 * @param f Pointer to the file.
 * @param offset Byte offset from the specified origin.
 * @param whence Position reference (e.g., SEEK_SET, SEEK_CUR, SEEK_END).
 * @param error Pointer to store error code if the operation fails.
 */
void compio_seek(compio_file* f, long offset, int whence, compio_errcode* error);

/**
 * @brief Gets the current position of the file pointer.
 *
 * @param f Pointer to the file.
 * @return The current position within the file.
 */
long compio_tell(compio_file* f);

/**
 * @brief Closes a file within the archive.
 *
 * @param f Pointer to the file to be closed.
 * @return 0 on success, or a non-zero error code on failure.
 */
int compio_close_file(compio_file* f);

/**
 * @brief Closes a compressed archive.
 *
 * @param a Pointer to the archive to be closed.
 * @return 0 on success, or a non-zero error code on failure.
 */
int compio_close_archive(compio_archive* a);

#endif // COMPIO_COMPIO_H
