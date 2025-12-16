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

struct context_t {
    FILE *file;
    block_allocator *allocator;
    btree *index;
    const compio_compressor *compressor;
    /**
     * @brief Temporary in-memory substitution for BTree, to avoid excess BTree operations
     *
     * When storage_block_reader::create_block creates new block, it inserts new key-value pair
     * into btree, but it sets tree_val::addr to zero, since we didn't call allocator::allocate
     * yet. This allocation happens only in block destructor, because we don't know resulting
     * size after compression before block destructor. Then, after that allocation,
     * btree::update is called, to update tree_val::addr in btree to an actual address.
     *
     * That by itself seems fine, since if this block is still in cache (no btree::update
     * happened yet, and tree_val::addr=0), then when storage_block_reader::read_block receives
     * addr=0, it gets this block from cache, so we don't need to read it from file. And on the
     * other hand, if block is no longer in cache, it means it's destructor got called, because
     * otherwise it would mean that something like this is happening:
     *
     * ```
     * block b1 = storage_block_reader::create_block(size, k1); // b1 added to cache
     * block b2 = storage_block_reader::read_block(a2, k2); // b1 evicted from cache
     * uint64_t b1_addr = btree.get(k1).addr; // it is actually 0, because b1 destructor was not
     * called yet storage_block_reader::read_block(b1_addr, k1); // b1 not in cache, but addr=0
     * is passed as an argument
     * ```
     *
     * But this situation is weird, because why would we need to read block, that we already
     * have? So it seems like there could be no problems.
     *
     * But the problem appears, because of how we use storage_block_reader in compio_write:
     * firstly, we get many tree_vals from btree via btree::get_range, then we iterate through
     * them and call storage_block_reader::read_block on their keys and addresses. But as we
     * iterating through blocks, we update cache, and some blocks get evicted, which calls
     * btree::update in block destructor. And if some block in the end of btree::get_range output
     * got evicted before compio_write processed it, then compio_write will call
     * storage_block_reader::read_block with invalid address and key, that is not in cache.
     *
     * That problem could be solved by making btree::get call in each iteration, since it's
     * tree_val could be no longer valid. But since we don't want unnecessary btree calls for
     * perfomance reasons, we use temporary_index, which stores actual addresses from block
     * destructor. Key benefit of this approach is that we can clear that temporary index in the
     * end of compio_write/read, so temporary_index is not big, and operations are faster that
     * on btree.
     */
    std::map<tree_key, uint64_t> temporary_index;
    bool is_temporary_index_enabled;
};

/**
 * @brief Storage block with automatic compression and file management
 *
 * Represents a single storage block that can be read from file, created in memory,
 * and automatically compressed when written back to storage. The block handles
 * compression/decompression transparently and manages its own lifecycle through
 * RAII - modified blocks are automatically compressed and written to file when
 * the block is destroyed.
 *
 * Blocks are typically managed through storage_block_reader which provides
 * caching and handles the complex interaction between the block, file storage,
 * B-tree index, and memory allocator.
 */
class block {
    context_t &context;
    tree_key _key;
    uint64_t _addr;
    uint64_t _c_size;
    std::unique_ptr<uint8_t[]> _data;
    uint64_t _size;
    bool _is_modified;
    bool _is_removed;
    bool _is_valid;

public:
    /**
     * @brief Construct a block by reading from file
     *
     * Reads an existing block from the specified file address and decompresses
     * it if necessary. The block data is loaded into memory for manipulation.
     *
     * @param context Reference to the storage context containing file, allocator, index, and
     * compressor
     * @param key The tree key identifying this block
     * @param addr File address where the block is stored
     */
    block(context_t &context, const tree_key &key, uint64_t addr);

    /**
     * @brief Construct a new empty block in memory
     *
     * Creates a new block with the specified size, initially filled with uninitialized
     * data. The block is marked as modified and will be compressed and written to
     * file when destroyed.
     *
     * @param context Reference to the storage context containing file, allocator, index, and
     * compressor
     * @param key The tree key identifying this block
     * @param size Size of the block in bytes (uncompressed)
     * @param unused Unused parameter to distinguish from other constructor
     */
    block(context_t &context, const tree_key &key, uint64_t size, bool unused);

    /**
     * @brief Destructor - automatically compresses and writes modified blocks
     *
     * If the block has been modified and not removed, compresses the data using
     * the configured compressor, allocates file space, writes the compressed data,
     * and updates the B-tree index with the new file address. Also handles
     * temporary index updates if enabled.
     */
    ~block();

    /**
     * @brief Get read-only access to block data
     *
     * @return Const pointer to the uncompressed block data
     */
    const uint8_t *data() const;

    /**
     * @brief Get read-write access to block data
     *
     * Returns a non-const pointer to the block data, automatically marking
     * the block as modified. Any changes to the data will trigger compression
     * and file writing when the block is destroyed.
     *
     * @return Pointer to the uncompressed block data that can be modified
     */
    uint8_t *data();

    /**
     * @brief Check if the block is valid
     *
     * A block becomes invalid if decompression fails during construction. Invalid blocks should not
     * be used (this is handled by storage_block_reader).
     *
     * @return true if the block is valid and can be used, false otherwise
     */
    bool is_valid() const;

    /**
     * @brief Reduce the size of the block
     *
     * Shrinks the block to the specified new size. The block is marked as
     * modified and the B-tree index is updated with the new size.
     *
     * @param new_size The new size for the block (must be less than current size)
     */
    void shrink(uint64_t new_size);

    /**
     * @brief Mark the block for removal
     *
     * Sets the removal flag. When the block is destroyed, it will not be
     * written back to file and will be removed from the B-tree index.
     * The file space will be deallocated by the caller.
     */
    void remove();

    /**
     * @brief Shift the block's key by the specified amount
     *
     * Adjusts the block's key by adding the specified value. This is used
     * when inserting or deleting data that affects the position of subsequent
     * blocks. The block is marked as modified.
     *
     * @param addition The amount to add to the key (can be positive or negative, but not zero)
     */
    void shift_key(int64_t addition);

    /**
     * @brief Set a new key for the block
     *
     * Changes the block's key to the specified value. If the new key is different
     * from the current key, the block is marked as modified.
     *
     * @param new_key The new key to assign to this block
     */
    void set_key(const tree_key &new_key);

    /**
     * @brief Get the block's key
     *
     * @return Const reference to the tree key identifying this block
     */
    const tree_key &key() const;

    /**
     * @brief Get the uncompressed size of the block
     *
     * @return Size of the block in bytes when uncompressed
     */
    uint64_t size() const;

    /**
     * @brief Get the file address of the block
     *
     * @return File address where this block is stored (0 if not yet written)
     */
    uint64_t addr() const;

    /**
     * @brief Get the compressed size of the block
     *
     * @return Size of the block in bytes when compressed (0 if not yet compressed)
     */
    uint64_t c_size() const;
};

/**
 * @brief Storage block reader with LRU caching and temporary indexing
 *
 * Provides high-level management of storage blocks with automatic caching,
 * compression handling, and temporary indexing for performance optimization.
 * This class serves as the main interface for reading, creating, and managing
 * storage blocks in the compressed file system.
 *
 * The reader maintains an LRU cache of blocks to avoid repeated file I/O and
 * compression operations.
 *
 * When temporary index is enabled, caller is allowed to pass expired addresses to read_block if and
 * only if this address became expired after enabling of temporary index (which means, that block
 * destructor saved actual address into temporary index in destructor). This is used for optimized
 * processing of range of blocks (obtained from btree::get_range), without pulling their
 * actual addresses from btree before processing every single block.
 *
 * Key features:
 * - LRU caching of blocks with configurable size
 * - Automatic compression/decompression through the block class
 * - Temporary indexing to allow optimized processing of block range
 * - Range operations for key shifting
 */
class storage_block_reader {
    cache::lru_cache<tree_key, std::shared_ptr<block>, tree_key_comparator> cache;
    context_t context;

public:
    /**
     * @brief Construct a storage block reader
     *
     * Initializes the block reader with the necessary components for file
     * operations, memory management, indexing, and compression. Sets up the
     * LRU cache with the specified maximum size.
     *
     * @param file File handle for reading/writing storage blocks
     * @param allocator Memory allocator for managing file space
     * @param index B-tree index for block lookup and management
     * @param compressor Compression interface for block compression/decompression
     * @param max_size Maximum number of blocks to keep in the LRU cache
     */
    storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                         const compio_compressor *compressor, int max_size);

    /**
     * @brief Read a block from file or cache
     *
     * Attempts to retrieve a block from the LRU cache first. If not found,
     * reads the block from the specified file address. If temporary indexing
     * is enabled, checks the temporary index for updated addresses before
     * reading from file.
     *
     * @param addr File address where the block is stored (may be 0 if temporary index is used)
     * @param key Tree key identifying the block
     * @return Shared pointer to the block, or nullptr if the block is invalid
     */
    std::shared_ptr<block> read_block(uint64_t addr, tree_key key);

    /**
     * @brief Create a new block in memory
     *
     * Creates a new empty block with the specified size and key. The block
     * is added to the cache and an entry is inserted into the B-tree index
     * with address 0 (to be updated when the block is written to file).
     *
     * @param size Size of the new block in bytes (uncompressed)
     * @param key Tree key to identify the new block
     * @return Shared pointer to the newly created block
     */
    std::shared_ptr<block> create_block(uint64_t size, tree_key key);

    /**
     * @brief Clear all blocks from the LRU cache
     *
     * Forces all cached blocks to be written to file (if modified) and
     * removes them from the cache. This is useful when you want to ensure
     * all changes are persisted or when you need to free memory.
     */
    void clear_cache();

    /**
     * @brief Enable temporary indexing
     *
     * Activates the temporary index mechanism which stores updated file
     * addresses for blocks, that were expired during temporary index lifetime.
     */
    void enable_temporary_index();

    /**
     * @brief Disable temporary indexing and clear it
     *
     * Deactivates the temporary index mechanism and clears all stored
     * entries. This should be called after bulk operations are complete
     * to free the temporary memory.
     */
    void disable_temporary_index();

    /**
     * @brief Shift keys in a specified range by an offset
     *
     * Updates all block keys within the specified range by adding the
     * specified offset. This operation affects:
     * - Temporary index entries
     * - Cached blocks (updates their internal keys)
     * - Cache internal structure
     *
     * This is typically used when inserting or deleting data that affects
     * the position of subsequent blocks.
     *
     * @param addition The amount to add to each key in the range
     * @param key_min Minimum key (inclusive) for the range
     * @param key_max Maximum key (inclusive) for the range
     */
    void add_to_range(int64_t addition, const tree_key &key_min, const tree_key &key_max);

    /**
     * @brief Remove a block from the system
     *
     * Marks the specified block for removal, removes it from cache,
     * temporary index (if enabled), B-tree index, and deallocates its
     * file space. This is the proper way to permanently remove a block.
     *
     * @param block Shared pointer to the block to remove
     */
    void remove_block(std::shared_ptr<block> block);

    /**
     * @brief Check if a block is in the cache
     *
     * @param key Tree key of the block to check
     * @return true if the block is currently cached, false otherwise
     */
    bool cache_contains(const tree_key &key) const;
};

#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
/** Number of blocks in current archive (used in benchmarking) */
extern long long bm_n_blocks;
#endif

} // namespace compio

#endif // STORAGE_BLOCK_READER_HPP_
