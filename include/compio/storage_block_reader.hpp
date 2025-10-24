/**
 * @file storage_block_reader.hpp
 * @brief Storage block reading and caching
 */

#ifndef STORAGE_BLOCK_READER_HPP_
#define STORAGE_BLOCK_READER_HPP_

#include "compio/allocator.hpp"
#include "compio/file.hpp"
#include "compio/infile_object.hpp"

#include "third_party/lrucache.hpp"

namespace compio {

/**
 * @brief Reader for storage blocks with LRU caching
 *
 * Provides efficient reading of storage blocks from archive file
 * with automatic caching to reduce disk I/O operations.
 */
struct storage_block_reader {
    /**
     * @brief Initialize block reader
     * @param file File handle to read from
     * @param max_size Maximum number of blocks to cache
     */
    storage_block_reader(FILE *file, block_allocator *allocator, int max_size);

    /**
     * @brief Read storage block from file
     * @param addr Block address in file
     * @return Smart pointer to storage block
     */
    smart_infile_object<storage_block> read_block(uint64_t addr);

    /**
     * @brief Create new storage block
     * @param addr Block address in file
     * @param data Block data
     * @param size Data size
     * @return Smart pointer to created block
     */
    smart_infile_object<storage_block>
    create_block(uint64_t addr, std::unique_ptr<uint8_t[]> &&data, uint64_t size);

    /**
     * @brief Remove block from cache
     * @param addr Block address to remove
     */
    void remove_block(uint64_t addr);

    /**
     * @brief Clear all cached blocks
     */
    void clear_cache();

private:
    FILE *file; /**< Archive file handle */
    cache::lru_cache<uint64_t, smart_infile_object<storage_block>>
        cache; /**< LRU cache for blocks */
    block_allocator *allocator;
};

} // namespace compio

#endif // STORAGE_BLOCK_READER_HPP_