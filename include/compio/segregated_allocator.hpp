/**
 * @file segregated_allocator.hpp
 * @brief Segregated Free List implementation for compio allocator
 *
 * This is a prototype implementation that can be integrated into the
 * existing free_blocks_manager to improve allocation performance.
 *
 * Key design decisions:
 * 1. Maintain both SFL (for fast allocation) and offset-sorted list (for merging)
 * 2. Use bucket sizes optimized for compressed blocks (256B to 64KB)
 * 3. Provide backward-compatible interface
 */

#ifndef COMPIO_SEGREGATED_ALLOCATOR_HPP
#define COMPIO_SEGREGATED_ALLOCATOR_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace compio {

/**
 * @brief Segregated Free List for fast block allocation
 *
 * Buckets are sized for typical compressed block sizes:
 * [0-256], [257-512], [513-1K], [1K-2K], [2K-4K], [4K-8K], [8K-16K], [16K-32K], [32K-64K], [>64K]
 */
class segregated_free_list {
public:
    static constexpr size_t NUM_BUCKETS = 10;

    struct free_block {
        uint64_t offset;
        uint64_t size;
        free_block* next_in_bucket;  // For SFL bucket list
        free_block* prev_in_bucket;
        free_block* next_by_offset;  // For offset-sorted list (needed for merging)
        free_block* prev_by_offset;
    };

    segregated_free_list();
    ~segregated_free_list();

    // Disable copy
    segregated_free_list(const segregated_free_list&) = delete;
    segregated_free_list& operator=(const segregated_free_list&) = delete;

    /**
     * @brief Add a free block to the manager
     * Attempts to merge with adjacent blocks first.
     */
    void add_free_block(uint64_t offset, uint64_t size);

    /**
     * @brief Allocate block using best-fit strategy
     */
    uint64_t allocate_best_fit(uint64_t size);

    /**
     * @brief Allocate block using first-fit strategy
     */
    uint64_t allocate_first_fit(uint64_t size);

    /**
     * @brief Allocate block using worst-fit strategy
     */
    uint64_t allocate_worst_fit(uint64_t size);

    /**
     * @brief Get total free space
     */
    uint64_t total_free() const { return total_free_; }

    /**
     * @brief Get number of free blocks
     */
    size_t block_count() const { return block_count_; }

    /**
     * @brief Check if region is already free
     */
    bool is_region_free(uint64_t offset, uint64_t size) const;

    /**
     * @brief Serialize to buffer
     */
    uint32_t serialize(std::vector<uint8_t>& buffer) const;

    /**
     * @brief Deserialize from buffer
     */
    bool deserialize(const uint8_t* buffer, uint32_t size);

    /**
     * @brief Clear all blocks
     */
    void clear();

    /**
     * @brief Print bucket statistics for debugging
     */
    void print_stats() const;

private:
    std::array<free_block*, NUM_BUCKETS> buckets_;
    free_block* head_by_offset_;
    free_block* tail_by_offset_;
    uint64_t total_free_;
    size_t block_count_;

    size_t get_bucket_index(uint64_t size) const;
    void insert_to_offset_list(free_block* block);
    void insert_to_bucket(free_block* block);
    void remove_from_bucket(free_block* block);
    void remove_from_offset_list(free_block* block);
    void find_mergeable_blocks(uint64_t offset, uint64_t size,
                               free_block*& prev, free_block*& next) const;
    void merge_blocks(free_block* prev, uint64_t offset, uint64_t size, free_block* next);
    uint64_t allocate_from_block(free_block* block, uint64_t size);
};

} // namespace compio

#endif // COMPIO_SEGREGATED_ALLOCATOR_HPP
