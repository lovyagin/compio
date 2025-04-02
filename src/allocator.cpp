/**
 * @file allocator.cpp
 * @brief Implementation of block allocation management
 */

#include <cinttypes>
#include "compio_file.hpp"
#include "allocator.hpp"
#include "file.hpp"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <utils.hpp>
#include <cassert>
#include <vector>

namespace compio {

    free_blocks_manager::free_blocks_manager(uint64_t* file_size)
    : head_(nullptr), tail_(nullptr), last_alloc_(nullptr),
      total_free_(0), file_size_(file_size) {
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
            for (free_block* current = head_; current; current = current->next) {
                if (current->size >= size && (!worst || current->size > worst->size)) {
                    worst = current;
                }
            }
            target = worst;
            break;
        }

        case allocation_strategy::NEXT_FIT: {
            if (!last_alloc_) last_alloc_ = head_;
            free_block* start = last_alloc_;
            free_block* current = start;

            do {
                if (current && current->size >= size) {
                    target = current;
                    last_alloc_ = current;
                    break;
                }
                current = current ? current->next : head_;
            } while (current && current != start);
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
    std::cout << "DEFRAG: Starting with validation checks" << std::endl;

    // Validate head state
    if (!head_) {
        std::cout << "DEFRAG: Empty list, nothing to do" << std::endl;
        return;
    }

    // Validate linked list integrity before starting
    std::cout << "DEFRAG: Validating list integrity" << std::endl;
    free_block* slow = head_;
    free_block* fast = head_;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            std::cout << "DEFRAG: CRITICAL - Circular reference detected!" << std::endl;
            // Break the circle at this point
            tail_ = slow->prev;
            if (tail_) tail_->next = nullptr;
            return;
        }
    }

    // Validate all pointers both ways
    free_block* current = head_;
    free_block* prev = nullptr;
    while (current) {
        if (current->prev != prev) {
            std::cout << "DEFRAG: CRITICAL - Broken prev pointer at offset " << current->offset << std::endl;
            current->prev = prev; // Fix it
        }
        prev = current;
        current = current->next;
    }

    if (prev != tail_) {
        std::cout << "DEFRAG: CRITICAL - Tail pointer mismatch" << std::endl;
        tail_ = prev; // Fix it
    }

    // In-place defragmentation with extensive error checking
    std::cout << "DEFRAG: Starting merge phase" << std::endl;
    current = head_;
    while (current && current->next) {
        std::cout << "DEFRAG: Checking " << current->offset << "+" << current->size
                  << " vs " << current->next->offset << std::endl;

        // Carefully check if blocks are adjacent
        if (current->offset + current->size == current->next->offset) {
            std::cout << "DEFRAG: Merging adjacent blocks" << std::endl;

            // Store all the pointers we'll need
            free_block* to_delete = current->next;
            free_block* next_next = to_delete->next;

            // Merge the blocks
            current->size += to_delete->size;
            current->next = next_next;

            // Fix the backwards link
            if (next_next) {
                next_next->prev = current;
            } else {
                tail_ = current;
            }

            // Delete the redundant block
            delete to_delete;

            // Don't advance current - we may be able to merge more
        } else {
            // Move to next block
            current = current->next;
        }
    }

    // Reset allocation pointer
    last_alloc_ = head_;

    std::cout << "DEFRAG: Complete" << std::endl;
}

    void free_blocks_manager::print_list() const {
        free_block* current = head_;
        while (current) {
            std::cout << "Block: " << current->offset << ", " << current->size << "\n";
            current = current->next;
        }
    }

    uint8_t free_blocks_manager::calculate_fragmentation() const {
        if (!head_) return 0;

        size_t free_space = 0;
        size_t free_blocks = 0;
        free_block* current = head_;

        while (current) {
            free_space += current->size;
            free_blocks++;
            current = current->next;
        }

        if (free_space == 0) return 0;

        return static_cast<uint8_t>((free_blocks - 1) * 100 / free_blocks);
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
        return blocks_manager_.calculate_fragmentation();
    }

    block_allocator::block_allocator(compio_archive* archive)
    : archive_(archive),
    blocks_manager_(archive->header ? &archive->header->file_size : nullptr) {
        assert(archive_ != nullptr);
        assert(archive_->header != nullptr);
    }

    uint64_t block_allocator::allocate(uint64_t size) {
        if (!size) return UINT64_MAX;  // Can't allocate zero bytes

        // Try to find in free blocks first
        uint64_t offset = blocks_manager_.allocate_block(size,
            static_cast<allocation_strategy>(archive_->config->allocation_strategy));

        if (offset != UINT64_MAX) {
            return offset;
        }

        // Extend the file if no suitable block found
        offset = archive_->header->file_size;
        archive_->header->file_size += size;
        return offset;
    }

void block_allocator::deallocate(uint64_t offset, uint64_t size) {
        if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header) return;

        blocks_manager_.add_free_block(offset, size);

        if (archive_->config->fill_holes_with_zeros && archive_->file) {
            std::vector<uint8_t> zeros(size, 0);
            fseek(archive_->file, offset, SEEK_SET);
            fwrite(zeros.data(), 1, size, archive_->file);
            fflush(archive_->file);
        }
    }

    void block_allocator::maintenance() {
        uint8_t frag = get_fragmentation();
        if (frag > archive_->config->fragmentation_threshold) {
            blocks_manager_.defragment();
        }
    }

// Private methods

    bool block_allocator::needs_defragmentation() const {
        return blocks_manager_.calculate_fragmentation() >
               archive_->config->fragmentation_threshold;
    }

    void block_allocator::perform_defragmentation() {
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

        for (const auto& [key, val] : used_blocks) {
            if (val.addr == new_offset) {
                new_offset += val.size;
                continue;
            }

            std::vector<uint8_t> buffer(val.size);
            fseek(archive_->file, val.addr, SEEK_SET);
            fread(buffer.data(), 1, val.size, archive_->file);

            fseek(archive_->file, new_offset, SEEK_SET);
            fwrite(buffer.data(), 1, val.size, archive_->file);

            relocations.emplace_back(key, tree_val{new_offset, val.size});
            new_offset += val.size;
        }

        for (const auto& [key, new_val] : relocations) {
            if (!archive_->index->update(key, new_val)) {
                fprintf(stderr, "Failed to update B-tree for key (%" PRIu64 ", %" PRIu64 ")\n",
                        key.hash, key.pos);
            }
        }

        *blocks_manager_.get_file_size_ptr() = new_offset;
        blocks_manager_.add_free_block(new_offset, UINT64_MAX - new_offset);

        flush_header(archive_);
        last_fragmentation_ = blocks_manager_.calculate_fragmentation();
        printf("Defragmentation complete. New file size: %" PRIu64 "\n", new_offset);
    }

} // namespace compio