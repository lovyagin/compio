#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include "btree.hpp"
#include "compio.h"
#include "file.hpp"

// forward declaration
namespace compio { class btree; }

/**
 * @brief Opened archive
 *
 */
struct compio_archive {
    FILE* file;                  /**< Opened stdio FILE */
    const compio_config* config; /**< Compio configuration */
    compio::header* header;      /**< Read file header */
    compio::btree* index;

    /**
     * @brief Parsed open mode (1 - read, 2 - write, 4 - edit, don't clear
     * contents)
     */
    uint8_t mode_b;
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
