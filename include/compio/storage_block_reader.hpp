/**
 * @file storage_block_reader.hpp
 * @brief Storage block reading and caching
 */

#ifndef STORAGE_BLOCK_READER_HPP_
#define STORAGE_BLOCK_READER_HPP_

#include <memory>

#include "compio/allocator.hpp"
#include "compio/btree.hpp"
#include "compio/tree_types.hpp"

#include "third_party/lrucache.hpp"

namespace compio {

class block {
    FILE *file;
    block_allocator *allocator;
    btree *index;
    const compio_compressor *compressor;
    tree_key key;
    uint64_t addr;
    uint64_t c_size;
    std::unique_ptr<uint8_t[]> dec_data;
    uint64_t dec_size;
    bool is_modified;
    bool is_removed;
    bool is_valid;

public:
    block(FILE *file, block_allocator *allocator, btree *index, const compio_compressor *compressor,
          uint64_t addr);
    block(FILE *file, block_allocator *allocator, btree *index, const compio_compressor *compressor,
          tree_key key, uint64_t size, std::unique_ptr<uint8_t[]> &&data);
    block(FILE *file, block_allocator *allocator, btree *index, const compio_compressor *compressor,
          tree_key key, uint64_t size, bool initialize_with_zeros);
    ~block();

    const uint8_t *data() const;
    uint8_t *data();
    bool valid() const;
};

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
    storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                         const compio_compressor *compressor, int max_size);

    /**
     * @brief Read storage block from file
     * @param addr Block address in file
     * @return Smart pointer to storage block
     */
    std::shared_ptr<block> read_block(uint64_t addr, tree_key key);

    /**
     * @brief Create new storage block
     * @param addr Block address in file
     * @param data Block data
     * @param size Data size
     * @return Smart pointer to created block
     */
    std::shared_ptr<block> create_block(uint64_t size, tree_key key);

    /**
     * @brief Clear all cached blocks
     */
    void clear_cache();

private:
    FILE *file; /**< Archive file handle */
    block_allocator *allocator;
    btree *index;
    const compio_compressor *compressor;
    cache::lru_cache<tree_key, std::shared_ptr<block>, tree_key_hash> cache;
};

} // namespace compio

#endif // STORAGE_BLOCK_READER_HPP_