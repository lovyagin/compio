#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include <memory>

#include "compio/btree.hpp"
#include "compio/file.hpp"
#include "compio/infile_object.hpp"
#include "compio/storage_block_reader.hpp"
#include "compio.h"

// forward declaration
namespace compio {

class btree;

std::vector<std::pair<tree_key, tree_val>> get_range_in_file(compio_file *file, uint64_t size);

} // namespace compio

/**
 * @brief Opened archive
 *
 */
struct compio_archive {
    FILE *file;                                 /**< Opened stdio FILE */
    const compio_config *config;                /**< Compio configuration */
    smart_infile_object<compio::header> header; /**< Read file header */
    compio::btree *index;
    compio::storage_block_reader block_reader;

    /**
     * @brief Parsed open mode (1 - read, 2 - write, 4 - edit, don't clear
     * contents)
     */
    uint8_t mode_b;

    compio_archive(FILE *file, uint8_t mode_b, const compio_config *config);

    compio::block_allocator *allocator;

    std::unique_ptr<uint8_t[]> c_buffer;
    cache::lru_cache<uint64_t, std::pair<std::shared_ptr<uint8_t[]>, uint64_t>> dec_cache;
};

/**
 * @brief Opened file inside of an archive
 */
struct compio_file {
    compio_archive *archive;          /**< Opened archive */
    char name[COMPIO_FNAME_MAX_SIZE]; /**< Internal filename */
    uint64_t cursor;                  /**< File cursor */
    uint64_t size;                    /**< File size */
    uint64_t hash_tail;               /**< Last 8 bytes of hashed filename */
};

#endif // COMPIO_FILE_HEADER_
