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
    struct free_block;

    uint8_t ZEROS[4096] = {0};

    free_blocks_manager::free_blocks_manager(uint64_t* file_size)
    : head_(nullptr), tail_(nullptr), last_alloc_(nullptr),
    total_free_(0), file_size_(file_size), cached_fragmentation_(0) {
        assert(file_size_ != nullptr);
    }

    void free_blocks_manager::add_free_block(uint64_t offset, uint64_t size) {
        if (size == 0) return;

        // Check for mergeable blocks
        free_block *prev, *next;
        find_mergeable_blocks(offset, size, prev, next);

        if (prev || next) {
            // Merge blocks if possible
            merge_blocks(prev, offset, size, next);
        } else {
            // Create new block if no merging possible
            auto* new_block = new free_block{offset, size, nullptr, nullptr};
            insert_ordered_block(new_block);
        }

        total_free_ += size;
        recently_defragmented_ = false;
        update_fragmentation();
    }

    void free_blocks_manager::find_mergeable_blocks(uint64_t offset, uint64_t size,
                                                free_block*& prev, free_block*& next) const {
        prev = next = nullptr;

        for (free_block* current = head_; current; current = current->next) {
            if (current->offset + current->size == offset) {
                prev = current;
            }
            else if (offset + size == current->offset) {
                next = current;
            }

            if (prev && next) break;
        }
    }

    void free_blocks_manager::remove_block(free_block* block) {
        if (!block) return;

        size_idx_.remove(block);

        if (block->prev) {
            block->prev->next = block->next;
        } else {
            head_ = block->next;
        }

        if (block->next) {
            block->next->prev = block->prev;
        } else {
            tail_ = block->prev;
        }

        if (last_alloc_ == block) {
            last_alloc_ = block->next ? block->next : head_;
        }

        delete block;
    }

    void free_blocks_manager::insert_ordered_block(free_block* new_block) {
        if (!head_) {
            head_ = tail_ = last_alloc_ = new_block;
            size_idx_.insert(new_block);
            return;
        }

        if (new_block->offset < head_->offset) {
            new_block->next = head_;
            head_->prev = new_block;
            head_ = new_block;
            size_idx_.insert(new_block);
            return;
        }

        free_block* current = head_;
        while (current->next && current->next->offset < new_block->offset) {
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

        size_idx_.insert(new_block);
    }

    void free_blocks_manager::merge_blocks(free_block* prev, uint64_t offset, uint64_t size, free_block* next) {
        if (prev && next) {
            // Remove both blocks from the size index before merging
            size_idx_.remove(prev);
            size_idx_.remove(next);

            // Merge all three blocks
            prev->size += size + next->size;

            if (next->next) {
                next->next->prev = prev;
            } else {
                tail_ = prev;
            }

            prev->next = next->next;

            // Update last_alloc_ if it points to the block being deleted
            if (last_alloc_ == next) {
                last_alloc_ = prev;
            }

            delete next;

            // Add merged block to the index
            size_idx_.insert(prev);
        }
        else if (prev) {
            // Remove block from index before size modification
            size_idx_.remove(prev);

            // Merge with previous block only
            prev->size += size;

            // Add updated block to index
            size_idx_.insert(prev);
        }
        else if (next) {
            // Remove block from index before modification
            size_idx_.remove(next);

            // Merge with next block only
            next->offset = offset;
            next->size += size;

            // Add updated block to index
            size_idx_.insert(next);
        }
        else {
            // No blocks to merge with, create new block
            auto* new_block = new free_block{offset, size, nullptr, nullptr};
            insert_ordered_block(new_block);
        }
    }

    uint64_t free_blocks_manager::allocate_block(uint64_t size, allocation_strategy strategy) {
        if (size == 0 || !head_) return UINT64_MAX;

        free_block* target = nullptr;

        switch (strategy) {
            case allocation_strategy::FIRST_FIT:
                target = find_first_fit(size);
                break;
            case allocation_strategy::BEST_FIT:
                target = find_best_fit(size);
                break;
            case allocation_strategy::WORST_FIT:
                target = find_worst_fit(size);
                break;
            case allocation_strategy::NEXT_FIT:
                target = find_next_fit(size);
                break;
        }

        if (!target) return UINT64_MAX;

        const uint64_t allocated_offset = target->offset;

        // Update the size index before modifying the block
        size_idx_.remove(target);

        if (target->size > size) {
            // Split block: keep remainder in free list
            uint64_t new_offset = target->offset + size;
            uint64_t remaining_size = target->size - size;
            target->offset = new_offset;
            target->size = remaining_size;

            // Re-add the modified block to the size index
            size_idx_.insert(target);
        } else {
            // Use entire block: remove from free list
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

            if (last_alloc_ == target) {
                last_alloc_ = target->next ? target->next : head_;
            }

            delete target;
        }

        total_free_ -= size;
        recently_defragmented_ = false;
        update_fragmentation();

        return allocated_offset;
    }

    void free_blocks_manager::defragment() {
        if (!head_ || !head_->next) {
            return;  // Nothing to defragment
        }

        #ifdef DEBUG
        size_t original_count = std::distance(head_, nullptr);
        DEBUG_PRINT("Starting defragmentation. Original blocks: %zu\n", original_count);
        #endif

        // Clear size index before defragmentation
        size_idx_.clear();

        bool changes_made;
        do {
            changes_made = false;
            free_block* current = head_;

            while (current && current->next) {
                free_block* next = current->next;

                // Check if current block can be merged with the next one
                if (current->offset + current->size == next->offset) {
                    current->size += next->size;
                    remove_block(next);
                    changes_made = true;
                    continue;  // Continue with current block as it might merge with more
                }

                current = current->next;
            }
        } while (changes_made);  // Repeat while blocks can be merged

        // Rebuild size index after defragmentation
        for (free_block* current = head_; current; current = current->next) {
            size_idx_.insert(current);
        }

        recently_defragmented_ = true;
        last_alloc_ = head_;  // Reset next_fit pointer

        #ifdef DEBUG
        size_t new_count = std::distance(head_, nullptr);
        DEBUG_PRINT("Defragmentation complete. Blocks after: %zu\n", new_count);
        #endif
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
        if (!size) return false;
        if (file_size_ && offset >= *file_size_) return true;

        for (free_block* current = head_; current; current = current->next) {
            if (current->offset <= offset &&
                offset + size <= current->offset + current->size) {
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
        uint64_t total_gaps = 0;
        uint64_t largest_block = 0;
        uint64_t smallest_block = UINT64_MAX;
        uint64_t total_free_space = 0;

        free_block* current = head_;
        free_block* prev = nullptr;

        while (current) {
            block_count++;
            total_free_space += current->size;

            largest_block = std::max(largest_block, current->size);
            smallest_block = std::min(smallest_block, current->size);

            if (prev) {
                total_gaps += current->offset - (prev->offset + prev->size);
            }

            prev = current;
            current = current->next;
        }

        if (block_count == 1) {
            return 0;
        }

        double size_dispersion = static_cast<double>(largest_block - smallest_block) / largest_block;
        double gaps_ratio = static_cast<double>(total_gaps) / total_free_space;

        double fragmentation =
            0.4 * std::min(1.0, (block_count - 1) / 9.0) +  // max 10 blocks = 100%
            0.3 * size_dispersion +
            0.3 * std::min(1.0, gaps_ratio);

        return static_cast<uint8_t>(fragmentation * 100);
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
        return size_idx_.find_best_fit(size);
    }

    free_block* free_blocks_manager::find_worst_fit(const uint64_t size) const {
        return size_idx_.find_worst_fit(size);
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

    block_allocator::block_allocator(compio_archive* archive)
        : archive_(archive)
        , blocks_manager_(&archive->header->file_size)
        , last_fragmentation_(0) {
        if (!archive_) {
            throw std::runtime_error("Archive pointer is null");
        }
        if (!archive_->header) {
            throw std::runtime_error("Archive header is null");
        }

        // Set initial state size
        archive_->header->allocator_state_size = sizeof(header);

        // Ensure we have valid initial file size
        if (archive_->header->file_size < sizeof(header)) {
            archive_->header->file_size = sizeof(header);
        }
    }

    uint8_t block_allocator::get_fragmentation() const {
        return blocks_manager_.get_cached_fragmentation();
    }

    uint64_t block_allocator::allocate(uint64_t size) {
        if (size == 0) {
            return UINT64_MAX;
        }

        try {
            // Convert allocation strategy from config to internal enum
            allocation_strategy strategy;
            switch (archive_->config->allocation_strategy) {
                case COMPIO_ALLOC_BEST_FIT:
                    strategy = allocation_strategy::BEST_FIT;
                    break;
                case COMPIO_ALLOC_WORST_FIT:
                    strategy = allocation_strategy::WORST_FIT;
                    break;
                case COMPIO_ALLOC_NEXT_FIT:
                    strategy = allocation_strategy::NEXT_FIT;
                    break;
                default:
                    strategy = allocation_strategy::FIRST_FIT;
            }

            // Try to find space in existing free blocks
            uint64_t offset = blocks_manager_.allocate_block(size, strategy);
            if (offset != UINT64_MAX) {
                return offset;
            }

            // If no suitable free block found, allocate at the end
            offset = archive_->header->file_size;
            archive_->header->file_size += size;
            return offset;
        } catch (const std::exception& e) {
            std::cerr << "Allocation failed: " << e.what() << std::endl;
            return UINT64_MAX;
        }
    }

    void block_allocator::deallocate(uint64_t offset, uint64_t size) {
        if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header) {
            return;
        }

        if (offset + size > archive_->header->file_size) {
            return;
        }

        if (blocks_manager_.is_region_free(offset, size)) {
            return;
        }

        blocks_manager_.add_free_block(offset, size);

        if (archive_->config->fill_holes_with_zeros && archive_->file) {
            static constexpr size_t BUFFER_SIZE = 4096;
            static uint8_t zeros[BUFFER_SIZE] = {0};

            fseek(archive_->file, offset, SEEK_SET);

            size_t remaining = size;
            while (remaining > 0) {
                size_t write_size = std::min(remaining, BUFFER_SIZE);
                if (fwrite(zeros, 1, write_size, archive_->file) != write_size) {
                    break;
                }
                remaining -= write_size;
            }

            fflush(archive_->file);
        }
    }

    void block_allocator::maintenance() {
        uint8_t current_fragmentation = get_fragmentation();
        uint8_t threshold = archive_->config->fragmentation_threshold;

        if (current_fragmentation > threshold) {
            blocks_manager_.defragment();
            blocks_manager_.update_fragmentation();

            if (blocks_manager_.get_cached_fragmentation() >= current_fragmentation) {
                if (archive_->file && archive_->index) {
                    perform_defragmentation();
                }
            }
        }

        last_fragmentation_ = blocks_manager_.get_cached_fragmentation();
    }

// Private methods

    bool block_allocator::needs_defragmentation() const {
        return blocks_manager_.calculate_fragmentation() >
               archive_->config->fragmentation_threshold;
    }

    void block_allocator::perform_defragmentation() {
        static constexpr size_t MOVE_BUFFER_SIZE = 1024 * 1024;  // 1MB buffer
        static std::vector<uint8_t> move_buffer(MOVE_BUFFER_SIZE);

        std::vector<std::pair<tree_key, tree_val>> used_blocks;
        constexpr tree_key key_min{};
        tree_key key_max{};
        key_max.hash = UINT64_MAX;
        key_max.pos = UINT64_MAX;

        archive_->index->get_range(key_min, key_max, used_blocks);

        std::sort(used_blocks.begin(), used_blocks.end(),
            [](const auto& a, const auto& b) { return a.second.addr < b.second.addr; });

        uint64_t new_offset = sizeof(header);
        std::vector<std::pair<tree_key, tree_val>> relocations;

        size_t batch_size = 0;
        uint64_t last_source_end = 0;
        uint64_t last_target_end = 0;

        auto flush_relocations = [this, &relocations]() {
            if (!relocations.empty()) {
                for (const auto& [key, val] : relocations) {
                    archive_->index->update(key, val);
                }
                relocations.clear();
            }
        };

        for (const auto& [key, val] : used_blocks) {
            uint64_t block_size = STORAGE_BLOCK_METASIZE + val.size;

            if (val.addr == new_offset) {
                new_offset += block_size;
                continue;
            }

            if (val.addr != last_source_end || new_offset != last_target_end) {
                flush_relocations();
                batch_size = 0;
            }

            if (fseek(archive_->file, val.addr, SEEK_SET) != 0) {
                DEBUG_PRINT("[AL] Error seeking to source block at %lu\n", val.addr);
                continue;
            }

            if (fseek(archive_->file, new_offset, SEEK_SET) != 0) {
                DEBUG_PRINT("[AL] Error seeking to target position at %lu\n", new_offset);
                continue;
            }

            size_t remaining = block_size;
            while (remaining > 0) {
                size_t chunk_size = std::min(remaining, MOVE_BUFFER_SIZE);

                if (fread(move_buffer.data(), 1, chunk_size, archive_->file) != chunk_size) {
                    DEBUG_PRINT("[AL] Error reading block data at %lu\n", val.addr + block_size - remaining);
                    break;
                }

                if (fwrite(move_buffer.data(), 1, chunk_size, archive_->file) != chunk_size) {
                    DEBUG_PRINT("[AL] Error writing block data at %lu\n", new_offset + block_size - remaining);
                    break;
                }

                remaining -= chunk_size;
            }

            tree_val new_val = val;
            new_val.addr = new_offset;
            relocations.push_back({key, new_val});

            last_source_end = val.addr + block_size;
            last_target_end = new_offset + block_size;
            new_offset += block_size;
            batch_size++;

            if (batch_size >= 1000) {
                flush_relocations();
                batch_size = 0;
            }
        }

        flush_relocations();

        fflush(archive_->file);

        blocks_manager_ = free_blocks_manager(archive_->header->file_size ? &archive_->header->file_size : nullptr);

        if (new_offset < archive_->header->file_size) {
            blocks_manager_.add_free_block(new_offset, archive_->header->file_size - new_offset);
        } else {
            archive_->header->file_size = new_offset;
        }

        blocks_manager_.update_fragmentation();
        blocks_manager_.save_to_file(archive_);

        DEBUG_PRINT("Defragmentation complete. New file size: %" PRIu64 "\n", new_offset);
    }

} // namespace compio
