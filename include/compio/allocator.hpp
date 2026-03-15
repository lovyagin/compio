/**
 * @file allocator.hpp
 * @brief Memory allocation management for compressed blocks storage
 *
 * This file implements a sophisticated block allocation system for managing
 * storage space in compressed archives. It includes free block management,
 * multiple allocation strategies, and automatic defragmentation support.
 */

#ifndef COMPIO_ALLOCATOR_HPP
#define COMPIO_ALLOCATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "compio.h"

namespace compio {

/**
 * @brief Structure representing a free block in storage
 */
struct free_block {
    uint64_t offset;  /**< Block start offset in file */
    uint64_t size;    /**< Block size in bytes */
    free_block *next; /**< Pointer to next free block (offset-sorted list) */
    free_block *prev; /**< Pointer to previous free block (offset-sorted list) */
    free_block *next_in_bucket = nullptr; /**< Pointer to next block in size bucket */
    free_block *prev_in_bucket = nullptr; /**< Pointer to prev block in size bucket */
};

/**
 * @brief Free blocks management strategies
 *
 * Different strategies for finding suitable free blocks during allocation:
 * - FIRST_FIT: Fast, finds first block that fits (good for speed)
 * - BEST_FIT: Finds smallest block that fits (minimizes wasted space)
 * - WORST_FIT: Finds largest block that fits (leaves larger fragments)
 * - NEXT_FIT: Like FIRST_FIT but continues from last allocation (spreads allocations)
 */
enum class allocation_strategy {
    FIRST_FIT, /**< Allocate first suitable block found */
    BEST_FIT,  /**< Allocate smallest suitable block to minimize waste */
    WORST_FIT, /**< Allocate largest suitable block to avoid small fragments */
    NEXT_FIT   /**< Continue search from last allocation point */
};

/**
 * @brief Free blocks table manager
 *
 * Manages a linked list of free blocks in the archive storage.
 * Supports multiple allocation strategies (first-fit, best-fit, worst-fit, next-fit)
 * and provides automatic block merging to reduce fragmentation.
 *
 * The manager maintains:
 * - A doubly-linked list of free blocks sorted by offset
 * - A size-based index for efficient best-fit and worst-fit searches
 * - Fragmentation tracking and caching
 * - Serialization/deserialization for persistent storage
 */
class free_blocks_manager {
public:
    /**
     * @brief Initialize manager with storage parameters
     * @param file_size Pointer to total file size reference
     */
    explicit free_blocks_manager(const uint64_t *file_size);

    /**
     * @brief Destructor — frees all nodes in the linked list
     */
    ~free_blocks_manager();

    /**
     * @brief Move assignment operator — takes ownership of other's nodes
     */
    free_blocks_manager &operator=(free_blocks_manager &&other) noexcept;

    /**
     * @brief Add new free block to the storage
     * @param offset Block start offset
     * @param size Block size
     */
    void add_free_block(uint64_t offset, uint64_t size);

    /**
     * @brief Find and allocate suitable block
     * @param size Required block size
     * @param strategy Allocation strategy to use
     * @return Offset of allocated block or UINT64_MAX if not found
     */
    uint64_t allocate_block(uint64_t size, allocation_strategy strategy);

    /**
     * @brief Merge adjacent free blocks
     */
    void defragment();

    /**
     * @brief Print current free blocks list for debugging
     */
    void print_list() const;

    /**
     * @brief Statistics about free blocks fragmentation
     */
    struct fragmentation_stats {
        size_t num_free_regions;      /**< Number of separate free regions */
        size_t total_free_bytes;       /**< Total free space in bytes */
        size_t largest_free_region;    /**< Size of largest contiguous free block */
        size_t smallest_free_region;   /**< Size of smallest free block */
        double avg_free_region_size;   /**< Average size of free regions */
        uint8_t fragmentation_percent; /**< Overall fragmentation percentage (0-100) */
    };

    /**
     * @brief Get detailed fragmentation statistics
     * @return Structure with fragmentation metrics
     */
    fragmentation_stats get_fragmentation_stats() const;

    /**
     * @brief Get cached fragmentation level (lazy: recalculates only when state changed)
     * @return Fragmentation percentage (0-100)
     */
    uint8_t get_cached_fragmentation() const;

    /**
     * @brief Calculate current fragmentation level without caching
     * @return Fragmentation percentage (0-100)
     */
    uint8_t calculate_fragmentation() const;

    /**
     * @brief Mark the cached fragmentation value as stale (triggers recalculation on next read)
     */
    void update_fragmentation();

    /**
     * @brief Check if a region is already marked as free
     * @param offset Start offset of the region
     * @param size Size of the region
     * @return True if region is already in the free list
     */
    bool is_region_free(uint64_t offset, uint64_t size) const;

    /**
     * @brief Get pointer to file size reference
     * @return Raw pointer to managed file size
     */
    const uint64_t *get_file_size_ptr() const { return file_size_; }

    /**
     * @brief Update pointer to file size reference
     * @param file_size New pointer to file size
     */
    void update_file_size_ptr(const uint64_t *file_size) { file_size_ = file_size; }

    /**
     * @brief Serialize manager state to buffer
     * @param buffer Vector to store serialized data
     * @return Size of serialized data
     */
    uint32_t serialize(std::vector<uint8_t> &buffer);

    /**
     * @brief Deserialize manager state from buffer
     * @param buffer Source buffer with serialized data
     * @param size Buffer size
     * @return True if deserialization successful
     */
    bool deserialize(const uint8_t *buffer, uint32_t size);

    /**
     * @brief Save manager state to archive file
     * @param archive Target archive
     * @return True if save successful
     */
    bool save_to_file(compio_archive *archive);

    /**
     * @brief Load manager state from archive file
     * @param archive Source archive
     * @return True if load successful
     */
    bool load_from_file(compio_archive *archive);

private:
    /**
     * @brief Remove block from the list and deallocate memory
     * @param block Block to remove
     */
    void remove_block(free_block *block);

    /**
     * @brief Segregated size index for fast block lookups
     *
     * Uses size-class buckets instead of multimap for faster allocation:
     * - Bucket 0: 0-256 bytes
     * - Bucket 1: 257-512 bytes
     * - Bucket 2: 513-1KB
     * - Bucket 3: 1KB-2KB
     * - Bucket 4: 2KB-4KB
     * - Bucket 5: 4KB-8KB
     * - Bucket 6: 8KB-16KB
     * - Bucket 7: 16KB-32KB
     * - Bucket 8: 32KB-64KB
     * - Bucket 9: >64KB
     *
     * Performance improvement: 4-5x for best-fit, 15-30x for first-fit
     */
    struct size_index {
        static constexpr size_t NUM_BUCKETS = 10;
        free_block* buckets[NUM_BUCKETS] = {nullptr};

        static size_t get_bucket(uint64_t size) {
            if (size <= 256) return 0;
            if (size <= 512) return 1;
            if (size <= 1024) return 2;
            if (size <= 2048) return 3;
            if (size <= 4096) return 4;
            if (size <= 8192) return 5;
            if (size <= 16384) return 6;
            if (size <= 32768) return 7;
            if (size <= 65536) return 8;
            return 9;
        }

        void insert(free_block *block) {
            size_t bucket = get_bucket(block->size);
            block->next_in_bucket = buckets[bucket];
            block->prev_in_bucket = nullptr;
            if (buckets[bucket]) {
                buckets[bucket]->prev_in_bucket = block;
            }
            buckets[bucket] = block;
        }

        void remove(free_block *block) {
            size_t bucket = get_bucket(block->size);
            if (block->prev_in_bucket) {
                block->prev_in_bucket->next_in_bucket = block->next_in_bucket;
            } else {
                buckets[bucket] = block->next_in_bucket;
            }
            if (block->next_in_bucket) {
                block->next_in_bucket->prev_in_bucket = block->prev_in_bucket;
            }
            block->next_in_bucket = nullptr;
            block->prev_in_bucket = nullptr;
        }

        void clear() {
            for (size_t i = 0; i < NUM_BUCKETS; ++i) {
                buckets[i] = nullptr;
            }
        }

        free_block *find_best_fit(uint64_t size) const {
            size_t start_bucket = get_bucket(size);
            free_block* best = nullptr;

            for (size_t i = start_bucket; i < NUM_BUCKETS; ++i) {
                for (free_block* block = buckets[i]; block; block = block->next_in_bucket) {
                    if (block->size >= size) {
                        if (!best || block->size < best->size) {
                            best = block;
                            if (block->size == size) return best; // Exact fit
                        }
                    }
                }
                // If found in current bucket, it's the best fit
                if (best && i == start_bucket) return best;
            }
            return best;
        }

        free_block *find_worst_fit(uint64_t size) const {
            // Start from largest bucket
            for (int i = NUM_BUCKETS - 1; i >= 0; --i) {
                free_block* largest = nullptr;
                for (free_block* block = buckets[i]; block; block = block->next_in_bucket) {
                    if (block->size >= size) {
                        if (!largest || block->size > largest->size) {
                            largest = block;
                        }
                    }
                }
                if (largest) return largest;
            }
            return nullptr;
        }
    };

    size_index size_idx_;

    free_block *head_;                           /**< Head of free blocks list */
    free_block *tail_;                           /**< Tail of free blocks list */
    free_block *last_alloc_;                     /**< Last allocation position for NEXT_FIT */
    uint64_t total_free_;                        /**< Total free space in bytes */
    const uint64_t *file_size_;                  /**< Reference to total file size */
    mutable uint8_t cached_fragmentation_;       /**< Cached fragmentation level */
    mutable bool fragmentation_dirty_;           /**< True when cache needs recalculation */

    /**
     * @brief Find the first suitable block for allocation
     * @param size Required block size
     * @return Pointer to the first suitable block or nullptr if not found
     */
    free_block *find_first_fit(uint64_t size) const;

    /**
     * @brief Find the smallest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the best-fit block or nullptr if not found
     */
    free_block *find_best_fit(uint64_t size) const;

    /**
     * @brief Find the largest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the worst-fit block or nullptr if not found
     */
    free_block *find_worst_fit(uint64_t size) const;

    /**
     * @brief Find the next suitable block for allocation
     * @param size Required block size
     * @return Pointer to the next-fit block or nullptr if not found
     */
    free_block *find_next_fit(uint64_t size) const;

    /**
     * @brief Find blocks that can be merged with the given region
     * @param offset Start offset of the region
     * @param size Size of the region
     * @param prev Output parameter for previous mergeable block
     * @param next Output parameter for next mergeable block
     */
    void find_mergeable_blocks(uint64_t offset, uint64_t size, free_block *&prev,
                               free_block *&next) const;

    /**
     * @brief Insert new block in the ordered list
     * @param new_block Block to insert
     */
    void insert_ordered_block(free_block *new_block);

    /**
     * @brief Merge three blocks (prev + new + next)
     * @param prev Previous block
     * @param offset New block offset
     * @param size New block size
     * @param next Next block
     */
    void merge_blocks(free_block *prev, uint64_t offset, uint64_t size, free_block *next);
};

/**
 * @brief High-level interface for block allocation operations
 *
 * Provides a simplified interface for allocating and deallocating storage blocks
 * in compressed archives. Automatically handles:
 * - Block allocation using the configured strategy
 * - Block deallocation and free space tracking
 * - Automatic defragmentation when fragmentation exceeds threshold
 * - State persistence (save/load allocator state)
 *
 * This is the main interface used by the archive system for managing storage.
 */
class block_allocator {
public:
    /**
     * @brief Get current fragmentation level
     * @return Fragmentation percentage (0-100)
     */
    [[nodiscard]] uint8_t get_fragmentation() const;

    /**
     * @brief Get detailed fragmentation statistics
     * @return Structure with fragmentation metrics
     */
    [[nodiscard]] free_blocks_manager::fragmentation_stats get_fragmentation_stats() const;

    /**
     * @brief Initialize allocator with archive configuration
     * @param archive Pointer to opened archive
     */
    explicit block_allocator(compio_archive *archive);

    /**
     * @brief Allocate storage block
     * @param size Required block size
     * @return Offset of allocated block
     */
    uint64_t allocate(uint64_t size);

    /**
     * @brief Release storage block
     * @param offset Block start offset
     * @param size Block size
     */
    void deallocate(uint64_t offset, uint64_t size);

    /**
     * @brief Perform maintenance operations if needed
     */
    void maintenance();

    /**
     * @brief Force defragmentation unconditionally (ignores fragmentation threshold).
     * Use this for on-demand defragmentation via the public API.
     */
    void force_defragmentation();

    /**
     * @brief Save allocator state to archive
     * @param archive Target archive
     * @return True if save was successful
     */
    bool save_state(compio_archive *archive);

    /**
     * @brief Load allocator state from archive
     * @param archive Source archive
     * @return True if load was successful
     */
    bool load_state(compio_archive *archive);

    /**
     * @brief Update pointer to file size reference (used when header is relocated/reloaded)
     * @param file_size New pointer to file size
     */
    void update_file_size_ptr(const uint64_t *file_size) { blocks_manager_.update_file_size_ptr(file_size); }

private:
    compio_archive *archive_;            /**< Associated archive */
    free_blocks_manager blocks_manager_; /**< Free blocks manager */
    uint8_t last_fragmentation_;         /**< Last measured fragmentation */

    /**
     * @brief Check if defragmentation is needed
     * @return True if fragmentation exceeds the threshold
     */
    [[nodiscard]] bool needs_defragmentation() const;

    /**
     * @brief Perform defragmentation of the storage
     */
    void perform_defragmentation();

    friend class TestAllocatorAccess;
};

} // namespace compio

#ifdef __cplusplus
extern "C" {
#endif

typedef void *compio_allocator_handle;

/**
 * @brief Create a new block allocator
 * @param archive Pointer to the archive
 * @return Handle to the created allocator
 */
compio_allocator_handle compio_create_allocator(compio_archive *archive);

/**
 * @brief Destroy an existing block allocator
 * @param handle Handle to the allocator
 */
void compio_destroy_allocator(compio_allocator_handle handle);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_ALLOCATOR_HPP
