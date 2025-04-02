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

    auto* new_block = new free_block{offset, size, nullptr, nullptr};

    if (!head_) {
        head_ = tail_ = new_block;
    } else {
        free_block* current = head_;
        while (current && current->offset < offset) {
            current = current->next;
        }

        if (current) {
            new_block->next = current;
            new_block->prev = current->prev;
            if (current->prev) {
                current->prev->next = new_block;
            } else {
                head_ = new_block;
            }
            current->prev = new_block;
        } else {
            tail_->next = new_block;
            new_block->prev = tail_;
            tail_ = new_block;
        }
    }

    // Merge neighbors
    if (new_block->prev && new_block->prev->offset + new_block->prev->size == new_block->offset) {
        new_block->prev->size += new_block->size;
        new_block->prev->next = new_block->next;
        if (new_block->next) {
            new_block->next->prev = new_block->prev;
        } else {
            tail_ = new_block->prev;
        }
        delete new_block;
        new_block = new_block->prev;
    }

    if (new_block->next && new_block->offset + new_block->size == new_block->next->offset) {
        new_block->size += new_block->next->size;
        free_block* to_delete = new_block->next;
        new_block->next = to_delete->next;
        if (to_delete->next) {
            to_delete->next->prev = new_block;
        } else {
            tail_ = new_block;
        }
        delete to_delete;
    }

    total_free_ += size;
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
        bool merged = true;
        while (merged) {
            merged = false;
            free_block* current = head_;
            while (current) {
                if (current->next && current->offset + current->size == current->next->offset) {
                    merge_with_neighbors(current);
                    merged = true;
                    break;
                } else {
                    current = current->next;
                }
            }
        }
    }

    uint8_t free_blocks_manager::calculate_fragmentation() const {
        if(!head_ || !head_->next) return 0;

        uint64_t max_free = 0;
        uint64_t total = 0;
        for (const auto* blk = head_; blk; blk = blk->next) {
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
            const free_block* to_delete = block->next;
            block->next = to_delete->next;
            if(to_delete->next) to_delete->next->prev = block;
            delete to_delete;
        }
    }

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
        if(!last_alloc_) return find_first_fit(size);

        free_block* start = last_alloc_;
        free_block* current = start;

        do {
            if(current->size >= size) return current;
            current = current->next ? current->next : head_;
        } while(current && current != start);

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
        if (size == 0 || !archive_ || !archive_->header) return UINT64_MAX;

        uint64_t offset = blocks_manager_.allocate_block(size,
            static_cast<allocation_strategy>(archive_->config->allocation_strategy));

        if (offset == UINT64_MAX) {
            offset = archive_->header->file_size;
            archive_->header->file_size += size;
        }

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
        if (const uint8_t frag = blocks_manager_.calculate_fragmentation();
            frag > archive_->config->fragmentation_threshold) {
            perform_defragmentation();
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