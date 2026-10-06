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

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compio.h"

namespace compio {

class WalManager;

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
 * @brief Free space manager
 *
 * Keeps the free regions of the archive in two ordered indexes over the same
 * set: by offset (neighbour lookup for merging, address-ordered strategies) and
 * by size (best-fit and worst-fit lookup). Freeing a region, best fit and worst
 * fit are logarithmic in the number of free regions; first fit and next fit
 * walk the regions in address order.
 *
 * Adjacent free regions are merged as soon as they are freed, so the set never
 * holds two regions that touch.
 */
class free_blocks_manager {
public:
    /**
     * @brief Initialize manager with storage parameters
     * @param file_size Pointer to total file size reference
     */
    explicit free_blocks_manager(const uint64_t *file_size);

    /**
     * @brief Add new free block to the storage
     * @param offset Block start offset
     * @param size Block size
     * @param merged_offset If not null, receives the start of the free region the block became part of
     * @param merged_size If not null, receives the size of that region
     * @return False if the region is empty, wraps around or overlaps a region that is already free
     *         (nothing is added then)
     */
    bool add_free_block(uint64_t offset, uint64_t size, uint64_t *merged_offset = nullptr,
                        uint64_t *merged_size = nullptr);

    /**
     * @brief Find and allocate suitable block
     * @param size Required block size
     * @param strategy Allocation strategy to use
     * @return Offset of allocated block or UINT64_MAX if not found
     */
    uint64_t allocate_block(uint64_t size, allocation_strategy strategy);

    /**
     * @brief Merge adjacent free blocks and restart the next-fit search from the beginning
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
     * @return True if the whole region lies inside one free region
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
     * @brief Number of free regions
     */
    size_t free_region_count() const;

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
    using region_map = std::map<uint64_t, uint64_t>;

    region_map by_offset_;                              /**< Free regions: offset -> size */
    std::set<std::pair<uint64_t, uint64_t>> by_size_;   /**< The same regions as (size, offset) */
    uint64_t next_fit_cursor_ = 0;                      /**< Offset the next NEXT_FIT search starts from */
    uint64_t total_free_;                               /**< Total free space in bytes */
    const uint64_t *file_size_;                         /**< Reference to total file size */
    mutable uint8_t cached_fragmentation_;              /**< Cached fragmentation level */
    mutable bool fragmentation_dirty_;                  /**< True when cache needs recalculation */
    uint64_t spare_slot_offset_ = 0;                    /**< Alternate slot of the saved state (0 = none) */
    uint64_t spare_slot_size_ = 0;                      /**< Size of the alternate slot */

    region_map::iterator insert_region(region_map::iterator hint, uint64_t offset, uint64_t size);
    region_map::iterator erase_region(region_map::iterator it);
    void clear_regions();

    /** Lowest region that can hold @p size, or end() */
    region_map::iterator find_first_fit(uint64_t size);
    /** Smallest region that can hold @p size (the lowest one among equals), or end() */
    region_map::iterator find_best_fit(uint64_t size);
    /** Largest region if it can hold @p size (the lowest one among equals), or end() */
    region_map::iterator find_worst_fit(uint64_t size);
    /** First region at or after the previous allocation that can hold @p size, wrapping around; or end() */
    region_map::iterator find_next_fit(uint64_t size);
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
     * @param wal Pointer to WAL manager (optional, can be nullptr)
     */
    explicit block_allocator(compio_archive *archive, WalManager *wal = nullptr);

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
     * @brief Release storage block with maintenance control
     * @param offset Block start offset
     * @param size Block size
     * @param perform_maintenance Trigger maintenance (defragmentation) after deallocation?
     */
    void deallocate(uint64_t offset, uint64_t size, bool perform_maintenance);

    /**
     * @brief Perform maintenance operations if needed
     * @param at_checkpoint True if the caller publishes a header right after the
     *        call; only then may a compaction relocate the files table
     */
    void maintenance(bool at_checkpoint = false);

    /**
     * @brief Suspend automatic maintenance operations (e.g. during file removal)
     */
    void suspend_maintenance() { maintenance_suspended_++; }

    /**
     * @brief Resume automatic maintenance operations
     */
    void resume_maintenance() { 
        if (maintenance_suspended_ > 0) maintenance_suspended_--; 
        if (maintenance_suspended_ == 0) maintenance();
    }

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
    WalManager *wal_;                    /**< WAL manager for ensuring durability */
    free_blocks_manager blocks_manager_; /**< Free blocks manager */
    uint8_t last_fragmentation_;         /**< Last measured fragmentation */
    uint8_t compaction_floor_ = 0;       /**< Fragmentation left after the last compaction */
    uint64_t deallocate_count_ = 0;      /**< Counter to throttle maintenance checks */
    std::atomic<int> maintenance_suspended_{0};      /**< Maintenance suspension counter */
    uint64_t fs_block_size_ = 0;         /**< Filesystem block size, read on first use */
    bool hole_punching_supported_ = true; /**< Cleared after the first failed attempt */

    /**
     * @brief Give the storage under a freed region back to the filesystem
     *
     * Punches a hole where the platform supports it and falls back to
     * overwriting the region with zeros.
     */
    void release_to_filesystem(uint64_t offset, uint64_t size, uint64_t merged_offset,
                               uint64_t merged_size);

    /**
     * @brief Punch a hole over a freed region
     * @return False if hole punching is unavailable or failed
     */
    bool punch_hole(uint64_t offset, uint64_t size, uint64_t merged_offset, uint64_t merged_size);

    /**
     * @brief Check if defragmentation is needed
     * @return True if fragmentation exceeds the threshold
     */
    [[nodiscard]] bool needs_defragmentation() const;

    /**
     * @brief Perform defragmentation of the storage
     */
    void perform_defragmentation(bool relocate_files_table);

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
