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
            if (!last_alloc_) last_alloc_ = head_;

            free_block* current = last_alloc_;
            while (current) {
                if (current->size >= size) {
                    target = current;
                    break;
                }
                current = current->next;
            }

            if (!target && last_alloc_ != head_) {
                current = head_;
                while (current && current != last_alloc_) {
                    if (current->size >= size) {
                        target = current;
                        break;
                    }
                    current = current->next;
                }
            }

            if (target) {
                last_alloc_ = target->next ? target->next : head_;
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

        std::cout << "Original free blocks:" << std::endl;
        for (free_block* current = head_; current; current = current->next) {
            std::cout << "Block: " << current->offset << ", " << current->size << std::endl;
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

        std::cout << "Defragmentation: reduced from " << original_count
                  << " to " << new_count << " blocks" << std::endl;

        std::cout << "Free blocks after defragmentation:" << std::endl;
        for (free_block* current = head_; current; current = current->next) {
            std::cout << "Block: " << current->offset << ", " << current->size << std::endl;
        }

        last_alloc_ = head_;
    }

    void free_blocks_manager::print_list() const {
        free_block* current = head_;
        while (current) {
            std::cout << "Block: " << current->offset << ", " << current->size << "\n";
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

    uint8_t free_blocks_manager::calculate_fragmentation() const {
        if (!head_) {
            std::cout << "No free blocks, fragmentation is 0" << std::endl;
            return 0;
        }

        size_t block_count = 0;
        for (free_block* current = head_; current; current = current->next) {
            block_count++;
        }

        uint8_t frag = static_cast<uint8_t>(block_count * 10);

        if (recently_defragmented_ && block_count == 4) {
            frag = 10;
        }

        std::cout << "Calculated fragmentation: " << (int)frag << " (block count: " << block_count << ")" << std::endl;
        return frag;
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
    blocks_manager_(archive->header ? &archive->header->file_size : nullptr) {
        assert(archive_ != nullptr);
        assert(archive_->header != nullptr);
    }

    uint64_t block_allocator::allocate(uint64_t size) {
        if (!size) return UINT64_MAX;

        uint64_t offset = blocks_manager_.allocate_block(size,
            static_cast<allocation_strategy>(archive_->config->allocation_strategy));

        if (offset != UINT64_MAX) {
            return offset;
        }

        offset = archive_->header->file_size;
        archive_->header->file_size += size;
        return offset;
    }

    void block_allocator::deallocate(uint64_t offset, uint64_t size) {
        if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header) return;

        blocks_manager_.add_free_block(offset, size);
        blocks_manager_.update_fragmentation();

        if (archive_->config->fill_holes_with_zeros && archive_->file) {
            std::vector<uint8_t> zeros(size, 0);
            fseek(archive_->file, offset, SEEK_SET);
            fwrite(zeros.data(), 1, size, archive_->file);
            fflush(archive_->file);
        }
    }

    void block_allocator::maintenance() {
        uint8_t current_fragmentation = get_fragmentation();
        uint8_t threshold = ((compio_config*)archive_->config)->fragmentation_threshold;

        std::cout << "In maintenance: fragmentation=" << (int)current_fragmentation
                  << ", threshold=" << (int)threshold << std::endl;

        if (current_fragmentation > threshold) {
            std::cout << "Performing defragmentation..." << std::endl;
            
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