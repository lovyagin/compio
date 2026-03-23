/**
 * @file allocator.cpp
 * @brief Implementation of block allocation management
 */

#include "compio/allocator.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#include "compio/compio_file.hpp"

#ifdef _WIN32
#include <io.h>   // _chsize_s, _fileno
#else
#include <unistd.h> // ftruncate, fileno
#endif

#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"
#include "compio/file.hpp"
#include "compio/sha256.hpp"
#include "compio/utils.hpp"

namespace compio {
struct free_block;

free_blocks_manager::free_blocks_manager(const uint64_t *file_size)
    : head_(nullptr),
      tail_(nullptr),
      last_alloc_(nullptr),
      total_free_(0),
      file_size_(file_size),
      cached_fragmentation_(0),
      fragmentation_dirty_(true) {
    assert(file_size_ != nullptr);
}

free_blocks_manager::~free_blocks_manager() {
    free_block *current = head_;
    while (current) {
        free_block *next = current->next;
        delete current;
        current = next;
    }
    head_ = tail_ = last_alloc_ = nullptr;
}

free_blocks_manager &free_blocks_manager::operator=(free_blocks_manager &&other) noexcept {
    if (this != &other) {
        // Free existing nodes
        free_block *current = head_;
        while (current) {
            free_block *next = current->next;
            delete current;
            current = next;
        }
        // Take ownership
        head_ = other.head_;
        tail_ = other.tail_;
        last_alloc_ = other.last_alloc_;
        total_free_ = other.total_free_;
        file_size_ = other.file_size_;
        cached_fragmentation_ = other.cached_fragmentation_;
        fragmentation_dirty_ = other.fragmentation_dirty_;
        size_idx_ = other.size_idx_;
        // Nullify source
        other.head_ = other.tail_ = other.last_alloc_ = nullptr;
        other.total_free_ = 0;
        other.size_idx_.clear();
    }
    return *this;
}

void free_blocks_manager::add_free_block(uint64_t offset, uint64_t size) {
    if (size == 0)
        return;
    if (offset > UINT64_MAX - size) // overflow guard: offset + size would wrap
        return;

    // Check for mergeable blocks
    free_block *prev, *next;
    find_mergeable_blocks(offset, size, prev, next);

    if (prev || next) {
        // Merge blocks if possible
        merge_blocks(prev, offset, size, next);
    } else {
        // Create new block if no merging possible
        auto *new_block = new free_block{offset, size, nullptr, nullptr, nullptr, nullptr};
        insert_ordered_block(new_block);
    }

    total_free_ += size;
    fragmentation_dirty_ = true;
}

void free_blocks_manager::find_mergeable_blocks(uint64_t offset, uint64_t size, free_block *&prev,
                                                free_block *&next) const {
    prev = next = nullptr;

    for (free_block *current = head_; current; current = current->next) {
        if (current->offset + current->size == offset) {
            prev = current;
        } else if (offset + size == current->offset) {
            next = current;
        }

        if (prev && next)
            break;
    }
}

void free_blocks_manager::remove_block(free_block *block) {
    if (!block)
        return;

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

void free_blocks_manager::insert_ordered_block(free_block *new_block) {
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

    free_block *current = head_;
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

void free_blocks_manager::merge_blocks(free_block *prev, uint64_t offset, uint64_t size,
                                       free_block *next) {
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
    } else if (prev) {
        // Remove block from index before size modification
        size_idx_.remove(prev);

        // Merge with previous block only
        prev->size += size;

        // Add updated block to index
        size_idx_.insert(prev);
    } else if (next) {
        // Remove block from index before modification
        size_idx_.remove(next);

        // Merge with next block only
        next->offset = offset;
        next->size += size;

        // Add updated block to index
        size_idx_.insert(next);
    } else {
        // No blocks to merge with, create new block
        auto *new_block = new free_block{offset, size, nullptr, nullptr, nullptr, nullptr};
        insert_ordered_block(new_block);
    }
}

uint64_t free_blocks_manager::allocate_block(uint64_t size, allocation_strategy strategy) {
    if (size == 0 || !head_)
        return UINT64_MAX;

    free_block *target = nullptr;

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

    if (!target)
        return UINT64_MAX;

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
    fragmentation_dirty_ = true;

    return allocated_offset;
}

void free_blocks_manager::defragment() {
    if (!head_ || !head_->next) {
        return;
    }

    // Clear size index — pointers inside blocks become stale, but we rebuild
    // the index from scratch at the end of this function.
    size_idx_.clear();

    // The free-block list is always kept sorted by offset.  A single forward
    // pass is therefore sufficient to merge all adjacent blocks: if
    // current->offset + current->size == next->offset the two blocks are
    // contiguous and can be merged into one.  We stay at 'current' after each
    // merge so that the newly enlarged block is immediately tested against its
    // new successor (chain merges in one pass, O(n) overall).
    free_block *current = head_;
    while (current && current->next) {
        free_block *nxt = current->next;
        if (current->offset + current->size == nxt->offset) {
            current->size += nxt->size;
            current->next  = nxt->next;
            if (nxt->next) {
                nxt->next->prev = current;
            } else {
                tail_ = current;
            }
            if (last_alloc_ == nxt) {
                last_alloc_ = current;
            }
            delete nxt;
            // Do NOT advance: current may now be adjacent to its new successor.
        } else {
            current = current->next;
        }
    }

    // Rebuild size index from the merged list.
    for (free_block *b = head_; b; b = b->next) {
        size_idx_.insert(b);
    }

    last_alloc_ = head_;
    fragmentation_dirty_ = true;
}

void free_blocks_manager::print_list() const {
    free_block *current = head_;
    while (current) {
        DEBUG_PRINT("Block: %" PRIu64 ", %" PRIu64 "\n", current->offset, current->size);
        current = current->next;
    }
}

uint8_t free_blocks_manager::get_cached_fragmentation() const {
    if (fragmentation_dirty_) {
        cached_fragmentation_ = calculate_fragmentation();
        fragmentation_dirty_ = false;
    }
    return cached_fragmentation_;
}

free_blocks_manager::fragmentation_stats free_blocks_manager::get_fragmentation_stats() const {
    fragmentation_stats stats = {};

    if (!head_) {
        return stats;
    }

    size_t block_count = 0;
    uint64_t largest_block = 0;
    uint64_t smallest_block = UINT64_MAX;

    for (free_block *current = head_; current; current = current->next) {
        block_count++;
        largest_block = std::max(largest_block, current->size);
        smallest_block = std::min(smallest_block, current->size);
    }

    stats.num_free_regions = block_count;
    stats.total_free_bytes = total_free_;
    stats.largest_free_region = largest_block;
    stats.smallest_free_region = (block_count > 0) ? smallest_block : 0;
    stats.avg_free_region_size = (block_count > 0) ?
        static_cast<double>(total_free_) / block_count : 0.0;

    // Compute fragmentation inline from already-gathered values (avoids second list traversal).
    if (block_count <= 1 || total_free_ == 0) {
        stats.fragmentation_percent = 0;
    } else {
        double ext_frag = 1.0 - static_cast<double>(largest_block) /
                                    static_cast<double>(total_free_);
        double count_score = std::min(1.0, static_cast<double>(block_count - 1) / 99.0);
        stats.fragmentation_percent =
            static_cast<uint8_t>((0.8 * ext_frag + 0.2 * count_score) * 100);
    }

    return stats;
}

void free_blocks_manager::update_fragmentation() {
    fragmentation_dirty_ = true;
}

bool free_blocks_manager::is_region_free(uint64_t offset, uint64_t size) const {
    if (!size)
        return false;
    if (size > UINT64_MAX - offset)
        return false;

    for (free_block *current = head_; current; current = current->next) {
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
    uint64_t largest_block = 0;
    uint64_t total_free_space = 0;

    for (free_block *cur = head_; cur; cur = cur->next) {
        block_count++;
        total_free_space += cur->size;
        if (cur->size > largest_block) largest_block = cur->size;
    }

    if (block_count == 1 || total_free_space == 0) {
        return 0;
    }

    // External fragmentation: 0 = all free space is one contiguous block,
    // 1 = all free space is scattered in tiny fragments.
    double ext_frag = 1.0 - static_cast<double>(largest_block) /
                                static_cast<double>(total_free_space);

    // Secondary: normalised block count (100 blocks ≈ max penalty).
    double count_score = std::min(1.0, static_cast<double>(block_count - 1) / 99.0);

    double fragmentation = 0.8 * ext_frag + 0.2 * count_score;
    return static_cast<uint8_t>(fragmentation * 100);
}

uint32_t free_blocks_manager::serialize(std::vector<uint8_t> &buffer) {
    // Count the number of blocks in the linked list
    size_t block_count = 0;
    for (free_block *current = head_; current; current = current->next) {
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
    uint64_t *data_ptr = reinterpret_cast<uint64_t *>(buffer.data() + sizeof(uint64_t));
    for (free_block *current = head_; current; current = current->next) {
        *data_ptr++ = current->offset;
        *data_ptr++ = current->size;
    }

    return size_needed;
}

bool free_blocks_manager::deserialize(const uint8_t *buffer, uint32_t size) {
    if (size < sizeof(uint64_t)) {
        return false;
    }

    uint64_t count;
    memcpy(&count, buffer, sizeof(uint64_t));

    // Guard against integer overflow: count * 16 could wrap
    if (count > (UINT32_MAX - sizeof(uint64_t)) / (2 * sizeof(uint64_t))) {
        return false;
    }
    uint32_t expected_size = sizeof(uint64_t) + static_cast<uint32_t>(count) * 2 * sizeof(uint64_t);
    if (size < expected_size) {
        return false;
    }

    // Clear existing blocks
    while (head_) {
        free_block *temp = head_;
        head_ = head_->next;
        delete temp;
    }
    head_ = tail_ = last_alloc_ = nullptr;
    total_free_ = 0;
    size_idx_.clear();

    const uint64_t *data_ptr = reinterpret_cast<const uint64_t *>(buffer + sizeof(uint64_t));
    for (uint64_t i = 0; i < count; i++) {
        uint64_t offset = *data_ptr++;
        uint64_t block_size = *data_ptr++;

        // Skip invalid entries
        if (block_size == 0) continue;
        if (file_size_ && offset + block_size > *file_size_) continue;
        if (offset + block_size < offset) continue; // overflow

        add_free_block(offset, block_size);
    }

    // Verify no overlapping blocks (list is offset-sorted after add_free_block).
    for (free_block *cur = head_; cur && cur->next; cur = cur->next) {
        if (cur->offset + cur->size > cur->next->offset) {
            // Corrupted data: overlapping free blocks — reset to empty state.
            while (head_) {
                free_block *tmp = head_;
                head_ = head_->next;
                delete tmp;
            }
            head_ = tail_ = last_alloc_ = nullptr;
            total_free_ = 0;
            size_idx_.clear();
            fragmentation_dirty_ = true;
            return false;
        }
    }

    return true;
}

bool free_blocks_manager::save_to_file(compio_archive *archive) {
    if (!archive || !archive->file || !archive->header) {
        return false;
    }

    std::vector<uint8_t> buffer;
    uint32_t size = serialize(buffer);

    // Write allocator state at the current logical end of the file.
    // This ensures the region is accounted for in file_size and won't
    // be overwritten by future allocations.
    int64_t pos = static_cast<int64_t>(readonly(archive->header, header)->file_size);
    const auto hdr_size = static_cast<int64_t>(readonly(archive->header, header)->disk_size());
    if (pos < hdr_size) {
        pos = hdr_size;
    }

    compio::TransactionGuard txn(archive->wal.get());
    if (archive->wal) {
        if (!archive->wal->log_write(WalRecordType::ALLOCATOR, pos, buffer.data(), size)) {
            WARNING_PRINT("warning: WAL log_write failed in allocator.save_state\n");
            // txn destructor will rollback automatically
            return false;
        }
        // Commit transaction (with optional auto-checkpoint)
        if (!txn.commit(archive->file, archive->config.wal_max_size_bytes)) {
            WARNING_PRINT("warning: WAL commit failed in allocator.save_state\n");
            return false;
        }
    }

    if (fseek64(archive->file, pos, SEEK_SET) != 0) {
        WARNING_PRINT("warning: fseek returned error in allocator.save_state\n");
        return false;
    }

    // Write to archive file (buffered) - after WAL commit (Write-Ahead)
    DEBUG_PRINT("[W][allocator]addr=%" PRId64 ";size=%" PRIu32 "\n", pos, size);
    size_t written = fwrite(buffer.data(), 1, size, archive->file);
    if (written != size) {
        WARNING_PRINT(
            "warning: fwrite failed to write all bytes in allocator.save_state (%zu < %u)\n", written, size);
        return false;
    }
    archive->header->allocator_state_offset = static_cast<uint64_t>(pos);
    archive->header->allocator_state_size = size;
    archive->header->file_size = static_cast<uint64_t>(pos) + size;

    fflush(archive->file);

    return true;
}

bool free_blocks_manager::load_from_file(compio_archive *archive) {
    if (!archive || !archive->file || !archive->header) {
        return false;
    }

    // Check if allocator state exists
    if (readonly(archive->header, header)->allocator_state_offset == 0 ||
        readonly(archive->header, header)->allocator_state_size == 0) {
        return false;
    }

    // Seek to allocator state position
    if (fseek64(archive->file,
               static_cast<int64_t>(readonly(archive->header, header)->allocator_state_offset),
               SEEK_SET) != 0) {
        return false;
    }

    // Create buffer for reading data
    std::vector<uint8_t> buffer(readonly(archive->header, header)->allocator_state_size);

    // Read allocator state data
    size_t read = fread(buffer.data(), 1, readonly(archive->header, header)->allocator_state_size,
                        archive->file);
    if (read != readonly(archive->header, header)->allocator_state_size) {
        return false;
    }

    // Deserialize buffer into this manager
    return deserialize(buffer.data(), static_cast<uint32_t>(buffer.size()));
}

// Private helper methods

free_block *free_blocks_manager::find_first_fit(const uint64_t size) const {
    for (auto *blk = head_; blk; blk = blk->next) {
        if (blk->size >= size)
            return blk;
    }
    return nullptr;
}

free_block *free_blocks_manager::find_best_fit(const uint64_t size) const {
    return size_idx_.find_best_fit(size);
}

free_block *free_blocks_manager::find_worst_fit(const uint64_t size) const {
    return size_idx_.find_worst_fit(size);
}

free_block *free_blocks_manager::find_next_fit(const uint64_t size) const {
    if (!last_alloc_ || !head_)
        return find_first_fit(size);

    free_block *current = last_alloc_;
    while (current) {
        if (current->size >= size)
            return current;
        current = current->next;
    }

    current = head_;
    while (current && current != last_alloc_) {
        if (current->size >= size)
            return current;
        current = current->next;
    }

    return nullptr;
}

// block_allocator implementation

block_allocator::block_allocator(compio_archive *archive, WalManager *wal)
    : archive_(archive),
      wal_(wal),
      blocks_manager_(&readonly(archive->header, header)->file_size),
      last_fragmentation_(0) {
    if (!archive_) {
        throw std::runtime_error("Archive pointer is null");
    }
    if (!archive_->header) {
        throw std::runtime_error("Archive header is null");
    }

    // Don't set initial allocator_state_size here - it should remain 0 until first save
    // The header fields allocator_state_offset and allocator_state_size are initialized to 0
    // and will be set properly when save_state() is called

    // Ensure we reserve space for double-buffered header
    if (!(archive_->mode_b & mode_bit::r)) {
        uint64_t reserved_size = readonly(archive_->header, header)->reserved_size();
        if (readonly(archive_->header, header)->file_size < reserved_size) {
            archive_->header->file_size = reserved_size;
            if (wal_) {
                // TODO: Log this header size change to WAL if we had a HeaderUpdate record type,
                // but currently header updates are handled via double-buffering.
                // However, since we're modifying in-memory state that affects durability, 
                // we should be aware.
            }
        }
    }
}

uint8_t block_allocator::get_fragmentation() const {
    if (!archive_) return 0;
    std::lock_guard<std::recursive_mutex> lock(archive_->allocator_mutex);
    return blocks_manager_.get_cached_fragmentation();
}

free_blocks_manager::fragmentation_stats block_allocator::get_fragmentation_stats() const {
    if (!archive_) return {};
    std::lock_guard<std::recursive_mutex> lock(archive_->allocator_mutex);
    return blocks_manager_.get_fragmentation_stats();
}

uint64_t block_allocator::allocate(uint64_t size) {
    if (size == 0) {
        return UINT64_MAX;
    }

    std::lock_guard<std::recursive_mutex> lock(archive_->allocator_mutex);

    try {
        // Convert allocation strategy from config to internal enum
        allocation_strategy strategy;
        switch (archive_->config.allocation_strategy) {
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
            if (wal_) {
                // Log the free list change (allocation from free list)
                // When we allocate from free list, we remove a free block.
                // The FreeListUpdate record is designed to track additions/removals of free blocks.
                // However, the current WAL implementation focuses on data consistency.
                // Full allocator state recovery is handled by periodic snapshots.
                // For now, we rely on the fact that if we crash, we reload the last valid allocator state
                // from disk, which matches the consistent state of the archive.
            }
            return offset;
        }

        // If no suitable free block found, allocate at the end
        {
            std::lock_guard<std::mutex> h_lock(archive_->header_mutex);
            offset = readonly(archive_->header, header)->file_size;
            if (offset > UINT64_MAX - size) {
                return UINT64_MAX;
            }
            archive_->header->file_size += size;
        }
        return offset;
    } catch (const std::exception &e) {
        std::cerr << "Allocation failed: " << e.what() << std::endl;
        return UINT64_MAX;
    }
}

void block_allocator::deallocate(uint64_t offset, uint64_t size, bool perform_maintenance) {
    if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(archive_->allocator_mutex);
    
    // Validate bounds using the cached file size pointer to avoid locking header_mutex
    // (which could cause deadlocks if held by caller).
    const uint64_t *file_size_ptr = blocks_manager_.get_file_size_ptr();
    if (file_size_ptr) {
        if (size > UINT64_MAX - offset ||
            offset + size > *file_size_ptr) {
            return;
        }
    }

    if (blocks_manager_.is_region_free(offset, size)) {
        return;
    }

    blocks_manager_.add_free_block(offset, size);
    if (wal_) {
             // Similar to allocate, strict WAL logging of every free list change is expensive.
             // We rely on periodic checkpoints of the allocator state.
    }

    if (archive_->config.fill_holes_with_zeros && archive_->file) {
        static constexpr size_t BUFFER_SIZE = 4096;
        static uint8_t zeros[BUFFER_SIZE] = {0};

        DEBUG_PRINT("[W][deallocate]addr=%" PRIu64 ";size=%" PRIu64 "\n", offset, size);
        fseek64(archive_->file, offset, SEEK_SET);

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
    
    // Check if defragmentation is needed (throttled to avoid O(N) cost on every dealloc)
    if (perform_maintenance && maintenance_suspended_ == 0 && ++deallocate_count_ % 64 == 0) {
        maintenance();
    }
}

void block_allocator::force_defragmentation() {
    auto index_lock = archive_->index->get_lock();
    std::lock_guard<std::recursive_mutex> alloc_lock(archive_->allocator_mutex);

    blocks_manager_.defragment();
    if (archive_->file && archive_->index) {
        perform_defragmentation();
    }
    last_fragmentation_ = blocks_manager_.get_cached_fragmentation();
}

void block_allocator::maintenance() {
    {
        // Ensure the suspended flag is read under the same mutex used for allocator state
        std::lock_guard<std::recursive_mutex> alloc_lock(archive_->allocator_mutex);
        if (maintenance_suspended_ > 0) {
            return;
        }
    }

    uint8_t current_fragmentation = get_fragmentation();
    uint8_t threshold = archive_->config.fragmentation_threshold;

    if (current_fragmentation > threshold) {
        auto index_lock = archive_->index->try_get_lock();
        if (!index_lock.owns_lock()) {
             // If we can't lock the index, we skip defragmentation for now.
             // This avoids deadlocks when called from within a B-tree operation (which holds the lock).
             return;
        }
        std::lock_guard<std::recursive_mutex> alloc_lock(archive_->allocator_mutex);

        if (blocks_manager_.get_cached_fragmentation() > threshold) {
            blocks_manager_.defragment();

            if (blocks_manager_.get_cached_fragmentation() > threshold) {
                if (archive_->file && archive_->index) {
                    perform_defragmentation();
                }
            }
        }
        last_fragmentation_ = blocks_manager_.get_cached_fragmentation();
    } else {
        std::lock_guard<std::recursive_mutex> lock(archive_->allocator_mutex);
        last_fragmentation_ = blocks_manager_.get_cached_fragmentation();
    }
}

bool block_allocator::save_state(compio_archive *archive) {
    if (!archive) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> alloc_lock(archive->allocator_mutex);
    std::lock_guard<std::mutex> head_lock(archive->header_mutex);
    std::lock_guard<std::mutex> io_lock(archive->io_mutex);
    
    // Check if WAL is available to sync the allocator state update
    if (wal_) {
        // To be fully safe, we should ideally write the serialized allocator state 
        // through the WAL as a large raw write, or just rely on fsync.
        // Since the allocator state can be large, we typically don't put it *inside* the WAL log.
        // Instead we write it to the archive file and sync it.
        // But we MUST ensure any previous WAL entries are flushed first.
        wal_->sync();
    }
    
    bool result = blocks_manager_.save_to_file(archive);
    
    if (result && wal_) {
        // After writing allocator state to disk, we must ensure it hits physical storage
        // before we might update the header pointing to it.
        #ifdef _WIN32
        _commit(_fileno(archive->file));
        #else
        fsync(fileno(archive->file));
        #endif
    }
    
    return result;
}

bool block_allocator::load_state(compio_archive *archive) {
    if (!archive) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> alloc_lock(archive->allocator_mutex);
    std::lock_guard<std::mutex> head_lock(archive->header_mutex);
    std::lock_guard<std::mutex> io_lock(archive->io_mutex);
    return blocks_manager_.load_from_file(archive);
}

// Private methods

bool block_allocator::needs_defragmentation() const {
    return blocks_manager_.get_cached_fragmentation() > archive_->config.fragmentation_threshold;
}

void block_allocator::perform_defragmentation() {
    // Flush all cached/dirty blocks to disk first so that every index entry
    // has a real physical address before we start moving data.
    archive_->block_reader->set_maintenance_mode(true);
    archive_->block_reader->clear_cache();
    archive_->index->_clear_cache();

    // Collect B-tree node addresses so we don't overwrite them during compaction.
    // B-tree nodes and storage blocks share the same file address space.
    uint64_t btree_node_size = 0;
    auto node_addrs = archive_->index->_collect_node_addresses(btree_node_size);

    // Build a set for O(log n) lookup of reserved ranges.
    // Each node occupies [addr, addr + btree_node_size).
    auto overlaps_btree_node = [&](uint64_t start, uint64_t size) -> bool {
        // Binary search for the first node_addr <= start + size
        auto it = std::upper_bound(node_addrs.begin(), node_addrs.end(), start);
        // Check the node before (its range might extend into [start, start+size))
        if (it != node_addrs.begin()) {
            auto prev = std::prev(it);
            if (*prev + btree_node_size > start) return true;
        }
        // Check the node at/after start (it might start within [start, start+size))
        if (it != node_addrs.end() && *it < start + size) return true;
        return false;
    };

    constexpr size_t MOVE_BUFFER_SIZE = 1024 * 1024; // 1 MB
    std::vector<uint8_t> move_buffer(MOVE_BUFFER_SIZE);

    constexpr tree_key key_min{};
    tree_key key_max{};
    key_max.hash = UINT64_MAX;
    key_max.pos  = UINT64_MAX;

    auto used_blocks_opt = archive_->index->_get_range_impl(key_min, key_max);
    if (!used_blocks_opt) {
        WARNING_PRINT("defragmentation aborted: index read failed\n");
        return;
    }
    auto &used_blocks = *used_blocks_opt;

    std::sort(used_blocks.begin(), used_blocks.end(),
              [](const auto &a, const auto &b) { return a.second.addr < b.second.addr; });

    uint64_t write_pos = readonly(archive_->header, header)->reserved_size();

    // Collect final positions of placed storage blocks for gap computation.
    std::vector<std::pair<uint64_t, uint64_t>> placed_blocks;

    for (auto &[key, val] : used_blocks) {
        const uint64_t src = val.addr;

        if (src < readonly(archive_->header, header)->disk_size()) {
            continue;
        }

        // Read actual compressed size from on-disk metadata (signature + is_compressed + size).
        uint64_t compressed_size = 0;
        {
            const int64_t meta_offset = static_cast<int64_t>(src) + 2;
            if (fseek64(archive_->file, meta_offset, SEEK_SET) != 0 ||
                lendian_fread(&compressed_size, sizeof(compressed_size), 1, archive_->file) != 1 ||
                compressed_size == 0) {
                WARNING_PRINT("warning: perform_defragmentation: failed to read metadata at %" PRIu64 "\n", src);
                continue;
            }
        }
        const uint64_t block_size = STORAGE_BLOCK_METASIZE + compressed_size;

        while (overlaps_btree_node(write_pos, block_size)) {
            auto it = std::lower_bound(node_addrs.begin(), node_addrs.end(), write_pos);
            if (it != node_addrs.begin()) {
                auto prev = std::prev(it);
                if (*prev + btree_node_size > write_pos) {
                    write_pos = *prev + btree_node_size;
                    continue;
                }
            }
            if (it != node_addrs.end() && *it < write_pos + block_size) {
                write_pos = *it + btree_node_size;
                continue;
            }
            break;
        }

        if (src == write_pos) {
            placed_blocks.push_back({write_pos, block_size});
            write_pos += block_size;
            continue;
        }

        // Move the block chunk-by-chunk with explicit seeks (shared FILE*).
        // When dst > src (block moved forward past a B-tree node), use backward
        // copy to avoid corrupting overlapping source data.
        bool     io_error   = false;
        const bool backward = write_pos > src &&
                              write_pos < src + block_size; // regions overlap

        if (backward) {
            uint64_t src_cursor = src + block_size;
            uint64_t dst_cursor = write_pos + block_size;
            size_t   remaining  = block_size;

            while (remaining > 0) {
                size_t chunk = std::min(remaining, MOVE_BUFFER_SIZE);
                src_cursor -= chunk;
                dst_cursor -= chunk;

                if (fseek64(archive_->file, static_cast<int64_t>(src_cursor), SEEK_SET) != 0 ||
                    fread(move_buffer.data(), 1, chunk, archive_->file) != chunk) {
                    WARNING_PRINT("warning: perform_defragmentation: read failed at %" PRIu64 "\n",
                                  src_cursor);
                    io_error = true;
                    break;
                }

                if (fseek64(archive_->file, static_cast<int64_t>(dst_cursor), SEEK_SET) != 0 ||
                    fwrite(move_buffer.data(), 1, chunk, archive_->file) != chunk) {
                    WARNING_PRINT("warning: perform_defragmentation: write failed at %" PRIu64 "\n",
                                  dst_cursor);
                    io_error = true;
                    break;
                }

                remaining -= chunk;
            }
        } else {
            uint64_t src_cursor = src;
            uint64_t dst_cursor = write_pos;
            size_t   remaining  = block_size;

            while (remaining > 0) {
                size_t chunk = std::min(remaining, MOVE_BUFFER_SIZE);

                if (fseek64(archive_->file, static_cast<int64_t>(src_cursor), SEEK_SET) != 0 ||
                    fread(move_buffer.data(), 1, chunk, archive_->file) != chunk) {
                    WARNING_PRINT("warning: perform_defragmentation: read failed at %" PRIu64 "\n",
                                  src_cursor);
                    io_error = true;
                    break;
                }

                if (fseek64(archive_->file, static_cast<int64_t>(dst_cursor), SEEK_SET) != 0 ||
                    fwrite(move_buffer.data(), 1, chunk, archive_->file) != chunk) {
                    WARNING_PRINT("warning: perform_defragmentation: write failed at %" PRIu64 "\n",
                                  dst_cursor);
                    io_error = true;
                    break;
                }

                src_cursor += chunk;
                dst_cursor += chunk;
                remaining  -= chunk;
            }
        }

        if (io_error) {
            WARNING_PRINT("warning: perform_defragmentation aborted due to I/O error\n");
            return;
        }

        val.addr = write_pos;
        archive_->index->_update_impl(key, val);

        placed_blocks.push_back({write_pos, block_size});
        write_pos += block_size;
    }

    // Flush all updated index nodes to disk BEFORE truncating the file.
    // If we crash after truncation but before index write, we lose data.
    archive_->index->_clear_cache();

    if (fflush(archive_->file) != 0) {
        WARNING_PRINT("warning: perform_defragmentation: fflush failed\n");
        return;
    }
    #ifdef _WIN32
    if (_commit(_fileno(archive_->file)) != 0) {
        WARNING_PRINT("warning: perform_defragmentation: _commit failed\n");
        return;
    }
    #else
    if (fsync(fileno(archive_->file)) != 0) {
        WARNING_PRINT("warning: perform_defragmentation: fsync failed\n");
        return;
    }
    #endif

    // Compute safe truncation point: max of write_pos and end of last B-tree node.
    uint64_t truncate_pos = write_pos;
    if (!node_addrs.empty()) {
        uint64_t last_node_end = node_addrs.back() + btree_node_size;
        if (last_node_end > truncate_pos) {
            truncate_pos = last_node_end;
        }
    }

    // Physically truncate the file to the new (smaller) size so that the
    // freed tail space is actually returned to the OS.
#ifdef _WIN32
    if (_chsize_s(_fileno(archive_->file), static_cast<__int64>(truncate_pos)) != 0) {
        WARNING_PRINT("warning: perform_defragmentation: _chsize_s failed\n");
    }
#else
    if (ftruncate(fileno(archive_->file), static_cast<off_t>(truncate_pos)) != 0) {
        WARNING_PRINT("warning: perform_defragmentation: ftruncate failed\n");
    }
#endif

    // Update logical file_size in header and rebuild free-block manager.
    archive_->header->file_size = truncate_pos;

    blocks_manager_ = free_blocks_manager(&archive_->header->file_size);

    // Rebuild free blocks from gaps between all occupied regions (storage blocks + B-tree nodes).
    std::vector<std::pair<uint64_t, uint64_t>> occupied;
    occupied.reserve(placed_blocks.size() + node_addrs.size());
    for (auto &pb : placed_blocks) {
        occupied.push_back(pb);
    }
    for (uint64_t na : node_addrs) {
        if (na >= truncate_pos) break;
        occupied.push_back({na, btree_node_size});
    }
    std::sort(occupied.begin(), occupied.end());

    uint64_t scan = readonly(archive_->header, header)->disk_size() * 2;
    for (auto &[occ_start, occ_size] : occupied) {
        if (occ_start > scan) {
            blocks_manager_.add_free_block(scan, occ_start - scan);
        }
        uint64_t occ_end = occ_start + occ_size;
        if (occ_end > scan) {
            scan = occ_end;
        }
    }
    if (scan < truncate_pos) {
        blocks_manager_.add_free_block(scan, truncate_pos - scan);
    }

    // Invalidate the temporary index — it was populated with pre-move addresses
    // by clear_cache() above. After moving blocks the B-tree holds the correct
    // addresses; any stale temporary_index entries would cause reads to use
    // wrong offsets.
    archive_->block_reader->invalidate_temporary_index();
    archive_->block_reader->set_maintenance_mode(false);

    DEBUG_PRINT("[AL] perform_defragmentation complete. New file size: %" PRIu64 "\n", write_pos);
}

} // namespace compio
