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
#include "compio/compressor_set.hpp"
#include "compio/tree_types.hpp"

#include "lru_2q_cache.hpp"
#include "sharded_lru_2q_cache.hpp"

#include <mutex>
#include <unordered_set>
#include <atomic>

namespace compio {

class WalManager;
class block;

struct context_t {
    FILE *file;
    block_allocator *allocator;
    btree *index;
    compressor_set *compressors;
    std::mutex *io_mutex;
    WalManager *wal;
    compio_checksum_type checksum_type;
    // Range operations can hold addresses that become stale during cache eviction.
    // Successful write-back records replacements until the operation finishes.
    std::map<tree_key, uint64_t> temporary_index;
    std::atomic<int> temp_index_refcount = 0;
    std::mutex temp_index_mutex;
    // Blocks holding changes that are not written yet, so that a write-back
    // visits only those instead of the whole cache.
    std::mutex dirty_mutex;
    std::unordered_set<block *> dirty;

    void mark_dirty(block *b) {
        std::lock_guard<std::mutex> lock(dirty_mutex);
        dirty.insert(b);
    }
    void clear_dirty(block *b) {
        std::lock_guard<std::mutex> lock(dirty_mutex);
        dirty.erase(b);
    }

    context_t(const context_t&) = delete;
    context_t& operator=(const context_t&) = delete;

    context_t(FILE *file, block_allocator *allocator, btree *index,
              compressor_set *compressors, std::mutex *io_mutex, compio::WalManager *wal,
              compio_checksum_type checksum_type)
        : file(file),
          allocator(allocator),
          index(index),
          compressors(compressors),
          io_mutex(io_mutex),
          wal(wal),
          checksum_type(checksum_type) {}
};

/**
 * @brief Storage block with automatic compression and file management
 *
 * Represents a single storage block that can be read from file, created in memory,
 * and automatically compressed when written back to storage. The block handles
 * compression/decompression transparently.
 * Modified blocks are written explicitly by the reader before eviction or checkpoint.
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

    void set_modified() {
        _is_modified = true;
        context.mark_dirty(this);
    }
    uint32_t _codec = storage_block::codec_stored;

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
     * file by the reader.
     *
     * @param context Reference to the storage context containing file, allocator, index, and
     * compressor
     * @param key The tree key identifying this block
     * @param size Size of the block in bytes (uncompressed)
     * @param unused Unused parameter to distinguish from other constructor
     */
    block(context_t &context, const tree_key &key, uint64_t size, bool unused);

    /** @brief Codec the block was last read or written with */
    uint32_t codec() const { return _codec; }

    /** @brief Mark the block for writing back, so that it is packed again */
    void rewrite() { set_modified(); }

    // Write-back must be checked by the reader before destruction.
    ~block() { context.clear_dirty(this); }

    block(const block &) = delete;
    block &operator=(const block &) = delete;

    /**
     * @brief Write the block to the archive if it has unsaved modifications
     *
     * Compresses the data, places it at a newly allocated address (the old
     * place is released) and records that address in the index. The block
     * stays usable and becomes unmodified only on success.
     */
    bool write_back();

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
     * and file writing during eviction or flush.
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
     * @brief Increase the size of the block
     *
     * Increases the block to the specified new size by allocating 
     * new buffer of the desired size and copying data into it. 
     * The block is marked as modified and the B-tree index is updated with the new size.
     *
     * @param new_size The new size for the block (must be bigger than current size)
     */
    void grow(uint64_t new_size);

    /**
     * @brief Mark the block for removal
     *
     * Prevents further write-back. The caller removes the index entry
     * and deallocates the file space.
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
 * write-back saved the replacement address in the temporary index). This is used for optimized
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
    // Shard the block cache by file identity (tree_key::hash) so concurrent
    // reads of different files hit independent shards instead of serializing on
    // one cache mutex. Position shifts keep the same hash, so entries never
    // migrate shards.
    struct shard_by_hash {
        size_t operator()(const tree_key &k) const { return static_cast<size_t>(k.hash); }
    };
    // Declared before the cache: blocks unregister from it when destroyed.
    context_t context;
    cache::sharded_lru_2q_cache<tree_key, std::shared_ptr<block>, tree_key_comparator, shard_by_hash>
        cache;
    std::atomic<int> write_error{0};
    std::atomic<bool> flush_failed{false};

    bool prepare_eviction(const std::shared_ptr<block> &b);

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
     * @param compressors Compressors for block compression/decompression
     * @param max_size Maximum number of blocks to keep in the LRU cache
     */
    storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                         compressor_set *compressors, int max_size, std::mutex *io_mutex, compio::WalManager *wal,
                         compio_checksum_type checksum_type);

    storage_block_reader(const storage_block_reader &) = delete;
    storage_block_reader &operator=(const storage_block_reader &) = delete;
    storage_block_reader(storage_block_reader &&) = delete;
    storage_block_reader &operator=(storage_block_reader &&) = delete;

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
     * Writes modified blocks and removes unpinned entries on success.
     * Failed writes stay cached for retry.
     */
    bool clear_cache();

    /**
     * @brief Write all modified blocks to the archive, keeping them cached
     */
    bool flush_cache();

    bool has_write_error() const { return write_error.load() != 0 || flush_failed.load(); }
    bool cache_empty() const { return cache.size() == 0; }

    /**
     * @brief Set maintenance mode (unlocked updates)
     * 
     * @param enabled True to enable maintenance mode, false to disable
     */
    void set_maintenance_mode(bool enabled);

    /**
     * @brief Get cache hit probability
     * 
     * @return double Cache hit probability
     */
    double get_cache_hit_probability() const;

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
     * @brief Clear the temporary index without disabling it
     *
     * Removes all cached address entries from the temporary index while
     * keeping the mechanism active. Must be called after bulk address
     * changes (e.g. defragmentation) so subsequent reads use the
     * authoritative B-tree addresses instead of stale cached ones.
     */
    void invalidate_temporary_index();

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
     * @brief Rename a block in the cache and temporary index
     *
     * Updates the key of a block in the cache and temporary index.
     * This is used when a single block's key changes due to partial erasure.
     *
     * @param old_key The current key of the block
     * @param new_key The new key for the block
     * @param b The block itself (must be valid)
     */
    void rename_block(const tree_key &old_key, const tree_key &new_key, std::shared_ptr<block> b);

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

#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
/** Number of compressed bytes (used in benchmarking) */
extern long long bm_n_compressed_bytes;
/** Number of decompressed bytes (used in benchmarking) */
extern long long bm_n_decompressed_bytes;
#endif

} // namespace compio

#endif // STORAGE_BLOCK_READER_HPP_
