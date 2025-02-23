/**
 * @file allocator.cpp
 * @brief Implementation of block allocation management
 */

#include "compio_file.hpp"
#include "allocator.hpp"
#include "file.hpp"
#include <cstdio>
#include <algorithm>

namespace compio {

// free_blocks_manager implementation

    free_blocks_manager::free_blocks_manager(uint64_t* file_size)
            : head_(nullptr), tail_(nullptr), last_alloc_(nullptr),
              total_free_(0), file_size_(file_size) {}

    void free_blocks_manager::add_free_block(uint64_t offset, uint64_t size) {
        free_block* new_block = new free_block{offset, size, nullptr, nullptr};

        if(!head_) {
            head_ = tail_ = new_block;
        } else {
            // Insert sorted by offset
            free_block* current = head_;
            while(current && current->offset < offset) {
                current = current->next;
            }

            if(current) {
                new_block->next = current;
                new_block->prev = current->prev;
                if(current->prev) current->prev->next = new_block;
                else head_ = new_block;
                current->prev = new_block;
            } else {
                tail_->next = new_block;
                new_block->prev = tail_;
                tail_ = new_block;
            }
        }

        total_free_ += size;
        merge_with_neighbors(new_block);
    }

    uint64_t free_blocks_manager::allocate_block(uint64_t size, allocation_strategy strategy) {
        free_block* target = nullptr;

        switch(strategy) {
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
                // Implementation omitted for brevity
                break;
        }

        if(!target) return UINT64_MAX;

        // Allocate from the block
        const uint64_t allocated_offset = target->offset;

        if(target->size > size) {
            // Split the block
            target->offset += size;
            target->size -= size;
            total_free_ -= size;
        } else {
            // Remove entire block
            total_free_ -= target->size;
            if(target->prev) target->prev->next = target->next;
            else head_ = target->next;

            if(target->next) target->next->prev = target->prev;
            else tail_ = target->prev;

            delete target;
        }

        last_alloc_ = target;
        return allocated_offset;
    }

    void free_blocks_manager::defragment() {
        free_block* current = head_;
        while(current && current->next) {
            if(current->offset + current->size == current->next->offset) {
                current->size += current->next->size;
                free_block* to_delete = current->next;
                current->next = to_delete->next;
                if(to_delete->next) to_delete->next->prev = current;
                delete to_delete;
            } else {
                current = current->next;
            }
        }
    }

    uint8_t free_blocks_manager::calculate_fragmentation() const {
        if(!head_ || !head_->next) return 0;

        uint64_t max_free = 0;
        uint64_t total = 0;
        for(auto* blk = head_; blk; blk = blk->next) {
            max_free = std::max(max_free, blk->size);
            total += blk->size;
        }

        return static_cast<uint8_t>((1 - (max_free / static_cast<double>(total))) * 100);
    }

// Private helper methods

    void free_blocks_manager::merge_with_neighbors(free_block* block) {
        // Merge with previous
        if(block->prev && block->prev->offset + block->prev->size == block->offset) {
            block->prev->size += block->size;
            block->prev->next = block->next;
            if(block->next) block->next->prev = block->prev;
            delete block;
            block = block->prev;
        }

        // Merge with next
        if(block->next && block->offset + block->size == block->next->offset) {
            block->size += block->next->size;
            free_block* to_delete = block->next;
            block->next = to_delete->next;
            if(to_delete->next) to_delete->next->prev = block;
            delete to_delete;
        }
    }

    free_block* free_blocks_manager::find_first_fit(uint64_t size) const {
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size) return blk;
        }
        return nullptr;
    }

    free_block* free_blocks_manager::find_best_fit(uint64_t size) const {
        free_block* best = nullptr;
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size && (!best || blk->size < best->size)) {
                best = blk;
            }
        }
        return best;
    }

    free_block* free_blocks_manager::find_worst_fit(uint64_t size) const {
        free_block* worst = nullptr;
        for(auto* blk = head_; blk; blk = blk->next) {
            if(blk->size >= size && (!worst || blk->size > worst->size)) {
                worst = blk;
            }
        }
        return worst;
    }

// block_allocator implementation

    block_allocator::block_allocator(compio_archive* archive)
            : archive_(archive),
              blocks_manager_(archive->header ? &archive->header->file_size : nullptr),
              last_fragmentation_(0) {}

    uint64_t block_allocator::allocate(uint64_t size) {
        // Try to allocate from free blocks first
        const auto strategy = static_cast<allocation_strategy>(
                archive_->config->allocation_strategy
        );

        uint64_t offset = blocks_manager_.allocate_block(size, strategy);

        if (offset == UINT64_MAX) {
            uint64_t* file_size_ptr = blocks_manager_.get_file_size_ptr();
            if (!file_size_ptr) {
                return UINT64_MAX;
            }
            offset = *file_size_ptr;
            *file_size_ptr += size;
        }

        maintenance();
        return offset;
    }

    void block_allocator::deallocate(uint64_t offset, uint64_t size) {
        blocks_manager_.add_free_block(offset, size);

        if(archive_->config->fill_holes_with_zeros) {
            // Zero-fill implementation
            std::vector<uint8_t> zeros(size, 0);
            fseek(archive_->file, offset, SEEK_SET);
            fwrite(zeros.data(), 1, size, archive_->file);
        }

        maintenance();
    }

    void block_allocator::maintenance() {
        const uint8_t frag = blocks_manager_.calculate_fragmentation();
        if(frag > archive_->config->fragmentation_threshold) {
            perform_defragmentation();
        }
    }

// Private methods

    bool block_allocator::needs_defragmentation() const {
        return blocks_manager_.calculate_fragmentation() >
               archive_->config->fragmentation_threshold;
    }

    void block_allocator::perform_defragmentation() {
        // 1. Collect all used blocks through B-tree
        // 2. Reorganize blocks sequentially
        // 3. Update B-tree indexes
        // 4. Update free blocks list

        // Implementation details would require B-tree integration
        printf("Performing defragmentation...\n");
        blocks_manager_.defragment();
        last_fragmentation_ = blocks_manager_.calculate_fragmentation();
    }

} // namespace compio