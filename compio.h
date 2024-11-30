//
// External API header
//

#ifndef COMPIO_COMPIO_H
#define COMPIO_COMPIO_H

#include <stdio.h>
#include <errno.h>


/**
 * @brief Struct for opened archive
 * 
 */
typedef struct {
    FILE* file;
    const char* mode;
    // ...
} compio_archive;


/**
 * @brief Struct for opened file in archive
 * 
 */
typedef struct {
    compio_archive* archive;
    const char* mode;
    size_t cursor;
    // ...
} compio_file;


/**
 * @brief Open archive
 * 
 * @param fp path to file
 * @param mode mode (https://en.cppreference.com/w/cpp/io/c/fopen)
 * @return compio_archive* 
 */
compio_archive* compio_open_archive(const char* fp, const char* mode);


/**
 * @brief Open file inside of an opened archive
 * 
 * @param a archive
 * @param fp path to file inside of archive
 * @param mode mode (https://en.cppreference.com/w/cpp/io/c/fopen)
 * @return compio_file* 
 */
compio_file* compio_open_file(compio_archive* a, const char* fp, const char* mode);


/**
 * @brief Write block of data to file
 * 
 * @param ptr pointer to data
 * @param size size in bytes
 * @param f file
 * @return size_t 
 */
size_t compio_write(const void* ptr, size_t size, compio_file* f);


/**
 * @brief Read block of data from file
 * 
 * @param ptr pointer to buffer
 * @param size size in bytes
 * @param f file
 * @return size_t 
 */
size_t compio_read(void* ptr, size_t size, compio_file* f);


#define COMP_SEEK_SET   0
#define COMP_SEEK_CUR   1
#define COMP_SEEK_END   2


/**
 * @brief Set current position inside of a file
 * 
 * @param f file
 * @param offset offset in bytes
 * @param origin position, used as reference for the offset
 * `origin` possible values:
 *  - COMP_SEEK_SET - offset is counted from the beginning of a file
 *  - COMP_SEEK_CUR - offset is counter from current position
 *  - COMP_SEEK_END - offset is counter from the end of a file
 * @return int
 */
int compio_seek(compio_file* f, long offset, int origin);


/**
 * @brief Get current position inside of a file
 * 
 * @param f file
 * @return long 
 */
long compio_tell(compio_file* f);


/**
 * @brief Close opened file
 * 
 * @param f file
 * @return int 
 */
int compio_close_file(compio_file* f);


/**
 * @brief Close opened archive
 * 
 * @param a archive
 * @return int 
 */
int compio_close_archive(compio_archive* a);


#endif //COMPIO_COMPIO_H
