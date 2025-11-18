/**
 * @file storage_block_reader.hpp
 * @brief Storage block reading and caching
 */

#ifndef STORAGE_BLOCK_READER_HPP_
#define STORAGE_BLOCK_READER_HPP_

#include <map>
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
    std::map<tree_key, uint64_t> &temporary_index;
    const bool &is_temporary_index_enabled;
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
          std::map<tree_key, uint64_t> &temporary_index, const bool &is_temporary_index_enabled,
          uint64_t addr);
    block(FILE *file, block_allocator *allocator, btree *index, const compio_compressor *compressor,
          std::map<tree_key, uint64_t> &temporary_index, const bool &is_temporary_index_enabled,
          tree_key key, uint64_t size, std::unique_ptr<uint8_t[]> &&data);
    block(FILE *file, block_allocator *allocator, btree *index, const compio_compressor *compressor,
          std::map<tree_key, uint64_t> &temporary_index, const bool &is_temporary_index_enabled,
          tree_key key, uint64_t size);
    ~block();

    const uint8_t *data() const;
    uint8_t *data();
    bool valid() const;
    uint64_t size() const;
    void shrink(uint64_t new_size);
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

    /**
     * @brief Enable temporary index, so that every cache-evicted block will write it's new address
     * to it, and read_block will search for new address before reading block
     *
     * Enable this, when you got many tree_vals from get_range, then iterate through them, but you
     * don't want to update every tree_val with btree::get before processing it (as it might expire
     * because of cache eviction)
     */
    void enable_temporary_index();

    /**
     * @brief Disable temporary intex and clear it
     *
     * Disable this when you're done with processing possibly expired tree_vals
     */
    void disable_temporary_index();

    void add_to_range(int64_t addition, const tree_key& key_min, const tree_key& key_max);

private:
    FILE *file; /**< Archive file handle */
    block_allocator *allocator;
    btree *index;
    const compio_compressor *compressor;
    cache::lru_cache<tree_key, std::shared_ptr<block>, tree_key_comparator> cache;

    /**
     * @brief Temporary in-memory substitution for BTree, to avoid excess BTree operations
     *
     * When storage_block_reader::create_block creates new block, it inserts new key-value pair into
     * btree, but it sets tree_val::addr to zero, since we didn't call allocator::allocate yet. This
     * allocation happens only in block destructor, because we don't know resulting size after
     * compression before block destructor. Then, after that allocation, btree::update is called, to
     * update tree_val::addr in btree to an actual address.
     *
     * That by itself seems fine, since if this block is still in cache (no btree::update happened
     * yet, and tree_val::addr=0), then when storage_block_reader::read_block receives addr=0, it
     * gets this block from cache, so we don't need to read it from file. And on the other hand, if
     * block is no longer in cache, it means it's destructor got called, because otherwise it would
     * mean that something like this is happening:
     *
     * ```
     * block b1 = storage_block_reader::create_block(size, k1); // b1 added to cache
     * block b2 = storage_block_reader::read_block(a2, k2); // b1 evicted from cache
     * uint64_t b1_addr = btree.get(k1).addr; // it is actually 0, because b1 destructor was not
     * called yet storage_block_reader::read_block(b1_addr, k1); // b1 not in cache, but addr=0 is
     * passed as an argument
     * ```
     *
     * But this situation is weird, because why would we need to read block, that we already have?
     * So it seems like there could be no problems.
     *
     * But the problem appears, because of how we use storage_block_reader in compio_write: firstly,
     * we get many tree_vals from btree via btree::get_range, then we iterate through them and call
     * storage_block_reader::read_block on their keys and addresses. But if storage_block_reader
     * cache was not empty before btree::get_range, then some of tree_vals might have addr=0
     * (because these blocks are in cache). But as we iterating through blocks, we update cache, and
     * some blocks may get evicted, which calls btree::update in block destructor. And if some block
     * in the end of btree::get_range output got evicted before compio_write processed it, then
     * compio_write will call storage_block_reader::read_block with addr=0 and key, that is not in
     * cache.
     *
     * Even more obscure version of this problem appears, when we've got existing block from
     * btree::get_range, that has some address A, then, as we iterate through range, this block got
     * evicted from cache, but if it was modified, it deallocates it's previous address A, and
     * allocates new address B, and writes itself into this address. But when we process this block
     * in our range loop, it has it's previous address A, which is no longer valid.
     *
     * That problem could be solved by making btree::get call in each iteration, since it's tree_val
     * could be no longer valid. But since we don't want unnecessary btree calls for perfomance
     * reasons, we use temporary_index, which stores actual addresses from block destructor. Key
     * benefit of this approach is that we can clear that temporary index in the end of
     * compio_write/read, so temporary_index is not big, and operations are faster that on btree.
     */
    std::map<tree_key, uint64_t> temporary_index;
    bool is_temporary_index_enabled;
};

} // namespace compio

#endif // STORAGE_BLOCK_READER_HPP_