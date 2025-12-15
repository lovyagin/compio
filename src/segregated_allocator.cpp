/**
 * @file segregated_allocator.cpp
 * @brief Implementation of Segregated Free List for compio allocator
 */

#include "compio/segregated_allocator.hpp"

#include <cstdio>
#include <cstring>

namespace compio {

segregated_free_list::segregated_free_list()
    : head_by_offset_(nullptr),
      tail_by_offset_(nullptr),
      total_free_(0),
      block_count_(0) {
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        buckets_[i] = nullptr;
    }
}

segregated_free_list::~segregated_free_list() {
    // Free all blocks using offset list
    free_block* current = head_by_offset_;
    while (current) {
        free_block* next = current->next_by_offset;
        delete current;
        current = next;
    }
}

void segregated_free_list::add_free_block(uint64_t offset, uint64_t size) {
    if (size == 0) return;

    // Try to merge with adjacent blocks
    free_block* prev = nullptr;
    free_block* next = nullptr;
    find_mergeable_blocks(offset, size, prev, next);

    if (prev || next) {
        merge_blocks(prev, offset, size, next);
    } else {
        // Create new block
        auto* block = new free_block{offset, size, nullptr, nullptr, nullptr, nullptr};
        insert_to_offset_list(block);
        insert_to_bucket(block);
        block_count_++;
    }

    total_free_ += size;
}

uint64_t segregated_free_list::allocate_best_fit(uint64_t size) {
    if (size == 0) return UINT64_MAX;

    size_t start_bucket = get_bucket_index(size);
    free_block* best = nullptr;

    // Search from appropriate bucket upward
    for (size_t i = start_bucket; i < NUM_BUCKETS; ++i) {
        for (free_block* block = buckets_[i]; block; block = block->next_in_bucket) {
            if (block->size >= size) {
                if (!best || block->size < best->size) {
                    best = block;
                    // Exact fit - use immediately
                    if (block->size == size) {
                        return allocate_from_block(best, size);
                    }
                }
            }
        }
        // If found something in this bucket, it's best fit for this size class
        if (best && i == start_bucket) break;
    }

    if (!best) return UINT64_MAX;
    return allocate_from_block(best, size);
}

uint64_t segregated_free_list::allocate_first_fit(uint64_t size) {
    if (size == 0) return UINT64_MAX;

    size_t start_bucket = get_bucket_index(size);

    for (size_t i = start_bucket; i < NUM_BUCKETS; ++i) {
        for (free_block* block = buckets_[i]; block; block = block->next_in_bucket) {
            if (block->size >= size) {
                return allocate_from_block(block, size);
            }
        }
    }

    return UINT64_MAX;
}

uint64_t segregated_free_list::allocate_worst_fit(uint64_t size) {
    if (size == 0) return UINT64_MAX;

    // Start from largest bucket
    for (int i = NUM_BUCKETS - 1; i >= 0; --i) {
        free_block* largest = nullptr;
        for (free_block* block = buckets_[i]; block; block = block->next_in_bucket) {
            if (block->size >= size) {
                if (!largest || block->size > largest->size) {
                    largest = block;
                }
            }
        }
        if (largest) {
            return allocate_from_block(largest, size);
        }
    }

    return UINT64_MAX;
}

bool segregated_free_list::is_region_free(uint64_t offset, uint64_t size) const {
    for (free_block* current = head_by_offset_; current; current = current->next_by_offset) {
        if (current->offset <= offset &&
            offset + size <= current->offset + current->size) {
            return true;
        }
    }
    return false;
}

uint32_t segregated_free_list::serialize(std::vector<uint8_t>& buffer) const {
    size_t count = block_count_;
    uint32_t size_needed = sizeof(uint64_t) + count * 2 * sizeof(uint64_t);
    buffer.resize(size_needed);

    uint64_t block_count_val = count;
    memcpy(buffer.data(), &block_count_val, sizeof(uint64_t));

    uint64_t* data_ptr = reinterpret_cast<uint64_t*>(buffer.data() + sizeof(uint64_t));
    for (free_block* current = head_by_offset_; current; current = current->next_by_offset) {
        *data_ptr++ = current->offset;
        *data_ptr++ = current->size;
    }

    return size_needed;
}

bool segregated_free_list::deserialize(const uint8_t* buffer, uint32_t size) {
    if (size < sizeof(uint64_t)) return false;

    uint64_t count;
    memcpy(&count, buffer, sizeof(uint64_t));

    uint32_t expected = sizeof(uint64_t) + count * 2 * sizeof(uint64_t);
    if (size < expected) return false;

    // Clear existing
    clear();

    const uint64_t* data_ptr = reinterpret_cast<const uint64_t*>(buffer + sizeof(uint64_t));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t offset = *data_ptr++;
        uint64_t block_size = *data_ptr++;
        add_free_block(offset, block_size);
    }

    return true;
}

void segregated_free_list::clear() {
    free_block* current = head_by_offset_;
    while (current) {
        free_block* next = current->next_by_offset;
        delete current;
        current = next;
    }

    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        buckets_[i] = nullptr;
    }
    head_by_offset_ = nullptr;
    tail_by_offset_ = nullptr;
    total_free_ = 0;
    block_count_ = 0;
}

void segregated_free_list::print_stats() const {
    static const char* bucket_names[] = {
        "0-256B", "257-512B", "513B-1KB", "1-2KB", "2-4KB",
        "4-8KB", "8-16KB", "16-32KB", "32-64KB", ">64KB"
    };

    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        size_t count = 0;
        uint64_t total_size = 0;
        for (free_block* b = buckets_[i]; b; b = b->next_in_bucket) {
            count++;
            total_size += b->size;
        }
        if (count > 0) {
            printf("  Bucket %zu [%s]: %zu blocks, %lu bytes total\n",
                   i, bucket_names[i], count, total_size);
        }
    }
}

// Private methods

size_t segregated_free_list::get_bucket_index(uint64_t size) const {
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

void segregated_free_list::insert_to_offset_list(free_block* block) {
    if (!head_by_offset_ || block->offset < head_by_offset_->offset) {
        block->next_by_offset = head_by_offset_;
        block->prev_by_offset = nullptr;
        if (head_by_offset_) head_by_offset_->prev_by_offset = block;
        head_by_offset_ = block;
        if (!tail_by_offset_) tail_by_offset_ = block;
    } else {
        free_block* current = head_by_offset_;
        while (current->next_by_offset &&
               current->next_by_offset->offset < block->offset) {
            current = current->next_by_offset;
        }
        block->next_by_offset = current->next_by_offset;
        block->prev_by_offset = current;
        if (current->next_by_offset) {
            current->next_by_offset->prev_by_offset = block;
        } else {
            tail_by_offset_ = block;
        }
        current->next_by_offset = block;
    }
}

void segregated_free_list::insert_to_bucket(free_block* block) {
    size_t bucket = get_bucket_index(block->size);
    block->next_in_bucket = buckets_[bucket];
    block->prev_in_bucket = nullptr;
    if (buckets_[bucket]) {
        buckets_[bucket]->prev_in_bucket = block;
    }
    buckets_[bucket] = block;
}

void segregated_free_list::remove_from_bucket(free_block* block) {
    size_t bucket = get_bucket_index(block->size);

    if (block->prev_in_bucket) {
        block->prev_in_bucket->next_in_bucket = block->next_in_bucket;
    } else {
        buckets_[bucket] = block->next_in_bucket;
    }

    if (block->next_in_bucket) {
        block->next_in_bucket->prev_in_bucket = block->prev_in_bucket;
    }
}

void segregated_free_list::remove_from_offset_list(free_block* block) {
    if (block->prev_by_offset) {
        block->prev_by_offset->next_by_offset = block->next_by_offset;
    } else {
        head_by_offset_ = block->next_by_offset;
    }

    if (block->next_by_offset) {
        block->next_by_offset->prev_by_offset = block->prev_by_offset;
    } else {
        tail_by_offset_ = block->prev_by_offset;
    }
}

void segregated_free_list::find_mergeable_blocks(uint64_t offset, uint64_t size,
                                                  free_block*& prev, free_block*& next) const {
    prev = next = nullptr;

    for (free_block* current = head_by_offset_; current; current = current->next_by_offset) {
        if (current->offset + current->size == offset) {
            prev = current;
        } else if (offset + size == current->offset) {
            next = current;
        }
        if (prev && next) break;
    }
}

void segregated_free_list::merge_blocks(free_block* prev, uint64_t offset, uint64_t size, free_block* next) {
    if (prev && next) {
        // Remove both from buckets
        remove_from_bucket(prev);
        remove_from_bucket(next);

        // Merge all three
        prev->size += size + next->size;

        // Remove next from offset list
        remove_from_offset_list(next);
        delete next;
        block_count_--;

        // Re-add prev to correct bucket
        insert_to_bucket(prev);
    } else if (prev) {
        remove_from_bucket(prev);
        prev->size += size;
        insert_to_bucket(prev);
    } else if (next) {
        remove_from_bucket(next);
        next->offset = offset;
        next->size += size;
        insert_to_bucket(next);
    }
}

uint64_t segregated_free_list::allocate_from_block(free_block* block, uint64_t size) {
    uint64_t offset = block->offset;
    total_free_ -= size;

    // Remove from bucket
    remove_from_bucket(block);

    if (block->size > size) {
        // Split: update block and move to new bucket
        block->offset += size;
        block->size -= size;
        insert_to_bucket(block);
    } else {
        // Remove entirely
        remove_from_offset_list(block);
        delete block;
        block_count_--;
    }

    return offset;
}

} // namespace compio
