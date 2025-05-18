#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include "btree.hpp"
#include "compio.h"
#include "file.hpp"
#include "infile_object.hpp"
#include "storage_block_reader.hpp"

// forward declaration
namespace compio {
class btree;
}

/**
 * @brief Opened archive
 *
 */
struct compio_archive {
    FILE* file;                                 /**< Opened stdio FILE */
    const compio_config* config;                /**< Compio configuration */
    smart_infile_object<compio::header> header; /**< Read file header */
    compio::btree* index;
    compio::storage_block_reader block_reader;

    /**
     * @brief Parsed open mode (1 - read, 2 - write, 4 - edit, don't clear
     * contents)
     */
    uint8_t mode_b;

    compio_archive(FILE* file, uint8_t mode_b, const compio_config* config);

    compio::block_allocator* allocator;
};

/**
 * @brief Opened file inside of an archive
 */
struct compio_file {
    compio_archive* archive;          /**< Opened archive */
    char name[COMPIO_FNAME_MAX_SIZE]; /**< Internal filename */
    uint64_t cursor;                  /**< File cursor */
    uint64_t size;                    /**< File size */
};

#endif // COMPIO_FILE_HEADER_
