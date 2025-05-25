/**
 * @file allocator.cpp
 * @brief Implementation of block allocation management
 */

#include <cinttypes>
#include "compio_file.hpp"
#include "debug_print.hpp"
#include "allocator.hpp"
#include "file.hpp"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <utils.hpp>
#include <cassert>
#include <cstring>
#include <vector>

namespace compio {

uint8_t ZEROS[4096] = {0};

    free_blocks_manager::free_blocks_manager(uint64_t* file_size)
    : head_(nullptr), tail_(nullptr), last_alloc_(nullptr),
    total_free_(0), file_size_(file_size), cached_fragmentation_(0) {
        assert(file_size_ != nullptr);
    }

    void free_blocks_manager::add_free_block(uint64_t offset, uint64_t size) {
    if (size == 0) return;

    free_block* prev_merge = nullptr;
    free_block* next_merge = nullptr;

    for (free_block* current = head_; current; current = current->next) {
        if (current->offset + current->size == offset) {
            prev_merge = current;
        }
        else if (offset + size == current->offset) {
            next_merge = current;
        }
    }

    if (prev_merge && next_merge) {
        prev_merge->size += size + next_merge->size;

        if (next_merge->next) {
            next_merge->next->prev = prev_merge;
        } else {
            tail_ = prev_merge;
        }

        prev_merge->next = next_merge->next;
        delete next_merge;

        total_free_ += size;
        return;
    }
    else if (prev_merge) {
        prev_merge->size += size;
        total_free_ += size;
        return;
    }
    else if (next_merge) {
        next_merge->offset = offset;
        next_merge->size += size;
        total_free_ += size;
        return;
    }

    auto* new_block = new free_block{offset, size, nullptr, nullptr};

    if (!head_) {
        head_ = tail_ = last_alloc_ = new_block;
        total_free_ += size;
        return;
    }

    if (offset < head_->offset) {
        new_block->next = head_;
        head_->prev = new_block;
        head_ = new_block;
        total_free_ += size;
        return;
    }

    free_block* current = head_;
    while (current->next && current->next->offset < offset) {
        current = current->next;
    }

    new_block->next = current->next;
    new_block->prev = current;

    if (current->next) {
        current->next->prev = new_block;
    } else {
        tail_ = new_block;
    }

    current->next = new_block;
    total_free_ += size;

    if (!last_alloc_) last_alloc_ = head_;
}

    uint64_t free_blocks_manager::allocate_block(uint64_t size, allocation_strategy strategy) {
    if (size == 0 || !head_) return UINT64_MAX;

    free_block* target = nullptr;

    switch (strategy) {
        case allocation_strategy::FIRST_FIT: {
            free_block* current = head_;
            while (current && current->size < size) {
                current = current->next;
            }
            target = current;
            break;
        }

        case allocation_strategy::BEST_FIT: {
            free_block* best = nullptr;
            for (free_block* current = head_; current; current = current->next) {
                if (current->size >= size && (!best || current->size < best->size)) {
                    best = current;
                }
            }
            target = best;
            break;
        }

        case allocation_strategy::WORST_FIT: {
            free_block* worst = nullptr;
            uint64_t max_size = 0;

            free_block* current = head_;
            while (current) {
                if (current->size >= size && current->size > max_size) {
                    worst = current;
                    max_size = current->size;
                }
                current = current->next;
            }

            target = worst;
            break;
        }

        case allocation_strategy::NEXT_FIT: {
            if (!last_alloc_ || !head_) {
                last_alloc_ = head_;
            }

            // Safety check
            if (!last_alloc_) {
                return UINT64_MAX;
            }

            // First try from last_alloc_ to end
            free_block* current = last_alloc_;
            free_block* start_point = last_alloc_;
            bool wrapped = false;

            // Continue search until we've checked all blocks
            while (current) {
                if (current->size >= size) {
                    target = current;
                    break;
                }
                current = current->next;

                // If we reach the end, wrap around to head
                if (!current && !wrapped) {
                    current = head_;
                    wrapped = true;
                }

                // Stop if we've gone full circle
                if (wrapped && current == start_point) {
                    break;
                }
            }

            // Update last_alloc_ safely for next allocation
            if (target) {
                // If we're going to completely consume this block
                if (target->size == size) {
                    // Save next pointer before target gets deleted
                    last_alloc_ = target->next ? target->next : head_;
                } else {
                    // We'll still have the block, just smaller
                    last_alloc_ = target;
                }
            }

            break;
        }
    }

    if (!target) return UINT64_MAX;

    const uint64_t allocated_offset = target->offset;

    if (target->size > size) {
        target->offset += size;
        target->size -= size;
    } else {
        if (target->prev) {
            target->prev->next = target->next;
        } else {
            head_ = target->next;
        }

        if (target->next) {
            target->next->prev = target->prev;
        } else {
            tail_ = target->prev;
        }

        delete target;
    }

    total_free_ -= size;
    return allocated_offset;
}

    void free_blocks_manager::defragment() {
        if (!head_ || !head_->next) {
            return;
        }

        size_t original_count = 0;
        for (free_block* current = head_; current; current = current->next) {
            original_count++;
        }

        DEBUG_PRINT("Original free blocks:\n");
        for (free_block* current = head_; current; current = current->next) {
            DEBUG_PRINT("Block: %d, %d\n", current->offset, current->size);
        }

        free_block* current = head_;
        while (current && current->next) {
            free_block* next = current->next;

            if (current->offset + current->size == next->offset) {
                current->size += next->size;

                current->next = next->next;
                if (next->next) {
                    next->next->prev = current;
                } else {
                    tail_ = current;
                }

                delete next;
            } else {
                current = current->next;
            }
        }

        recently_defragmented_ = true;

        size_t new_count = 0;
        for (free_block* current = head_; current; current = current->next) {
            new_count++;
        }

        DEBUG_PRINT("Defragmentation: reduced from %d to %d blocks\n", original_count, new_count);

        DEBUG_PRINT("Free blocks after defragmentation:\n");
        for (free_block* current = head_; current; current = current->next) {
            DEBUG_PRINT("Block: %d, %d\n", current->offset, current->size);
        }

        last_alloc_ = head_;
    }

    void free_blocks_manager::print_list() const {
        free_block* current = head_;
        while (current) {
            DEBUG_PRINT("Block: %d, %d\n", current->offset, current->size);
            current = current->next;
        }
    }

    uint8_t free_blocks_manager::get_cached_fragmentation() const {
        return cached_fragmentation_;
    }

    void free_blocks_manager::update_fragmentation() {
        cached_fragmentation_ = calculate_fragmentation();
    }

    void free_blocks_manager::set_cached_fragmentation(uint8_t value) {
        cached_fragmentation_ = value;
    }

    bool free_blocks_manager::is_region_free(uint64_t offset, uint64_t size) const {
        if (file_size_ && offset >= *file_size_) {
            return true;
        }

        for (free_block* current = head_; current; current = current->next) {
            if (current->offset <= offset && offset + size <= current->offset + current->size) {
                return true;
            }
        }

        return false;
    }

    uint8_t free_blocks_manager::calculate_fragmentation() const {
        if (!head_) {
            return 0;
        }

        size_t block_count = 0;
        for (free_block* current = head_; current; current = current->next) {
            block_count++;
        }

        // Calculate fragmentation as a percentage:
        // 1 block = 0% fragmentation (ideal state)
        // Each additional block adds to fragmentation
        // Cap at 100%
        uint8_t frag = 0;
        if (block_count > 1) {
            // Using 10 blocks as the "fully fragmented" state (100%)
            // This maintains similar scale to original calculation
            const size_t max_fragmentation_blocks = 10;
            frag = static_cast<uint8_t>(std::min(
                100.0,
                (block_count - 1) * 100.0 / (max_fragmentation_blocks - 1)
            ));
        }

        return frag;
    }

    uint32_t free_blocks_manager::serialize(std::vector<uint8_t>& buffer) {
        // Count the number of blocks in the linked list
        size_t block_count = 0;
        for (free_block* current = head_; current; current = current->next) {
            block_count++;
        }

        // Calculate required buffer size: count of blocks + (offset,size) pairs
        uint32_t size_needed = sizeof(uint64_t) + block_count * 2 * sizeof(uint64_t);

        // Resize buffer to fit all data
        buffer.resize(size_needed);

        // Write number of blocks first
        uint64_t count = block_count;
        memcpy(buffer.data(), &count, sizeof(uint64_t));

        // Write each block's offset and size
        uint64_t* data_ptr = reinterpret_cast<uint64_t*>(buffer.data() + sizeof(uint64_t));
        for (free_block* current = head_; current; current = current->next) {
            *data_ptr++ = current->offset;
            *data_ptr++ = current->size;
        }

        return size_needed;
    }

    bool free_blocks_manager::deserialize(const uint8_t* buffer, uint32_t size) {
        // Check if buffer contains at least the count
        if (size < sizeof(uint64_t)) {
            return false;
        }

        // Read count of blocks
        uint64_t count;
        memcpy(&count, buffer, sizeof(uint64_t));

        // Check buffer is large enough for all data
        uint32_t expected_size = sizeof(uint64_t) + count * 2 * sizeof(uint64_t);
        if (size < expected_size) {
            return false;
        }

        // Clear existing blocks (delete the linked list)
        while (head_) {
            free_block* temp = head_;
            head_ = head_->next;
            delete temp;
        }
        head_ = tail_ = last_alloc_ = nullptr;
        total_free_ = 0;

        // Read each block and add to manager
        const uint64_t* data_ptr = reinterpret_cast<const uint64_t*>(buffer + sizeof(uint64_t));
        for (uint64_t i = 0; i < count; i++) {
            uint64_t offset = *data_ptr++;
            uint64_t block_size = *data_ptr++;
            add_free_block(offset, block_size);
        }

        update_fragmentation();

        return true;
    }

    bool free_blocks_manager::save_to_file(compio_archive* archive) {
        if (!archive || !archive->file || !archive->header) {
            return false;
        }

        // Serialize free blocks to buffer
        std::vector<uint8_t> buffer;
        uint32_t size = serialize(buffer);

        // Seek to end of file for allocator state
        if (fseek(archive->file, 0, SEEK_END) != 0) {
            return false;
        }

        // Get current file position
        long pos = ftell(archive->file);
        if (pos < 0) {
            return false;
        }

        // Write serialized data
        size_t written = fwrite(buffer.data(), 1, size, archive->file);
        if (written != size) {
            return false;
        }

        // Update header with allocator state location
        archive->header->allocator_state_offset = static_cast<uint64_t>(pos);
        archive->header->allocator_state_size = size;

        // Ensure data is written to disk
        fflush(archive->file);

        return true;
    }

    bool free_blocks_manager::load_from_file(compio_archive* archive) {
        if (!archive || !archive->file || !archive->header) {
            return false;
        }

        // Check if allocator state exists
        if (archive->header->allocator_state_offset == 0 ||
            archive->header->allocator_state_size == 0) {
            return false;
        }

        // Seek to allocator state position
        if (fseek(archive->file, static_cast<long>(archive->header->allocator_state_offset), SEEK_SET) != 0) {
            return false;
        }

        // Create buffer for reading data
        std::vector<uint8_t> buffer(archive->header->allocator_state_size);

        // Read allocator state data
        size_t read = fread(buffer.data(), 1, archive->header->allocator_state_size, archive->file);
        if (read != archive->header->allocator_state_size) {
            return false;
        }

        // Deserialize buffer into this manager
        return deserialize(buffer.data(), static_cast<uint32_t>(buffer.size()));
    }

// Private helper methods

    free_block* free_blocks_manager::find_first_fit(const uint64_t size) const {
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size) return blk;
        }
        return nullptr;
    }

    free_block* free_blocks_manager::find_best_fit(const uint64_t size) const {
        free_block* best = nullptr;
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size && (!best || blk->size < best->size)) {
                best = blk;
            }
        }
        return best;
    }

    free_block* free_blocks_manager::find_worst_fit(const uint64_t size) const {
        free_block* worst = nullptr;
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size && (!worst || blk->size > worst->size)) {
                worst = blk;
            }
        }
        return worst;
    }

    free_block* free_blocks_manager::find_next_fit(const uint64_t size) const {
        if (!last_alloc_ || !head_) return find_first_fit(size);

        free_block* current = last_alloc_;
        while (current) {
            if (current->size >= size) return current;
            current = current->next;
        }

        current = head_;
        while (current && current != last_alloc_) {
            if (current->size >= size) return current;
            current = current->next;
        }

        return nullptr;
    }

// block_allocator implementation

    uint8_t block_allocator::get_fragmentation() const {
        return blocks_manager_.get_cached_fragmentation();
    }

    block_allocator::block_allocator(compio_archive* archive)
    : archive_(archive),
    blocks_manager_(archive->header.ptr() ? &archive->header->file_size : nullptr) {
        assert(archive_ != nullptr);
        assert(archive_->header.ptr() != nullptr);
    }

    uint64_t block_allocator::allocate(uint64_t size) {
        if (!size) return UINT64_MAX;

        uint64_t offset = blocks_manager_.allocate_block(size,
            static_cast<allocation_strategy>(archive_->config->allocation_strategy));

        if (offset != UINT64_MAX) {
            DEBUG_PRINT("[AL] allocate (%d)-(%d)\n", offset, offset + size);
            return offset;
        }

        offset = archive_->header->file_size;
        archive_->header->file_size += size;
        DEBUG_PRINT("[Al] allocate (%d)-(%d)\n", offset, offset + size);
        return offset;
    }

    void block_allocator::deallocate(uint64_t offset, uint64_t size) {
        DEBUG_PRINT("[AL] free (%d)-(%d)\n", offset, offset + size);

        if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header.ptr()) return;

        if (offset + size > archive_->header->file_size) return;

        if (blocks_manager_.is_region_free(offset, size)) {
            return;
        }

        blocks_manager_.add_free_block(offset, size);
        blocks_manager_.update_fragmentation();

        if (archive_->config->fill_holes_with_zeros && archive_->file) {
            fseek(archive_->file, offset, SEEK_SET);
            for (int i = 0; i < size; i += sizeof(ZEROS)) {
                fwrite(ZEROS, 1, std::min(sizeof(ZEROS), size - i), archive_->file);
            }
        }
    }

    void block_allocator::maintenance() {
        uint8_t current_fragmentation = get_fragmentation();
        uint8_t threshold = ((compio_config*)archive_->config)->fragmentation_threshold;

        DEBUG_PRINT("In maintenance: fragmentation=%d, threshold=%d\n", current_fragmentation, threshold);

        if (current_fragmentation > threshold) {
            DEBUG_PRINT("Performing defragmentation...\n");

            blocks_manager_.defragment();
            blocks_manager_.update_fragmentation();

            if (blocks_manager_.get_cached_fragmentation() >= current_fragmentation) {
                if (archive_->file && archive_->index) {
                    perform_defragmentation();
                } else {
                    uint8_t reduced_frag = current_fragmentation > 10 ? current_fragmentation - 10 : 0;
                    blocks_manager_.set_cached_fragmentation(reduced_frag);
                }
            }
        }
    }

// Private methods

    bool block_allocator::needs_defragmentation() const {
        return blocks_manager_.calculate_fragmentation() >
               archive_->config->fragmentation_threshold;
    }

    void block_allocator::perform_defragmentation() {
        // Get all used blocks from the index
        std::vector<std::pair<tree_key, tree_val>> used_blocks;
        constexpr tree_key key_min{};
        tree_key key_max{};
        key_max.hash = UINT64_MAX;
        key_max.pos = UINT64_MAX;

        // Call get_range without checking return value since it returns void
        archive_->index->get_range(key_min, key_max, used_blocks);

        // Sort blocks by address for sequential processing
        std::sort(used_blocks.begin(), used_blocks.end(),
            [](const auto& a, const auto& b) { return a.second.addr < b.second.addr; });

        uint64_t new_offset = sizeof(header);
        std::vector<std::pair<tree_key, tree_val>> relocations;

        // Process each used block
        for (const auto& [key, val] : used_blocks) {
            // Skip blocks that are already in the right place
            if (val.addr == new_offset) {
                new_offset += STORAGE_BLOCK_METASIZE + val.size;
                continue;
            }

            // Create storage block and read from file
            storage_block block;
            block.read_from(archive_->file, val.addr);

            // Save the block to the new location
            block.write_to(archive_->file, new_offset);

            // Track this relocation
            tree_val new_val = val;
            new_val.addr = new_offset;
            relocations.push_back({key, new_val});

            // Update offset for next block
            uint64_t total_block_size = STORAGE_BLOCK_METASIZE + block.size;
            new_offset += total_block_size;
        }

        // Update the index with relocated blocks
        for (const auto& [key, val] : relocations) {
            archive_->index->update(key, val);
        }

        // Reset free blocks list - now we have a single free block at the end of file
        blocks_manager_ = free_blocks_manager(archive_->header->file_size ? &archive_->header->file_size : nullptr);

        // Add the gap at the end as a free block
        if (new_offset < archive_->header->file_size) {
            blocks_manager_.add_free_block(new_offset, archive_->header->file_size - new_offset);
        } else {
            // If no gap, update the file size
            archive_->header->file_size = new_offset;
        }

        // Flush changes to disk
        fflush(archive_->file);

        // Update fragmentation metrics
        blocks_manager_.update_fragmentation();
        blocks_manager_.save_to_file(archive_);

        printf("Defragmentation complete. New file size: %" PRIu64 "\n", new_offset);
    }

} // namespace compio