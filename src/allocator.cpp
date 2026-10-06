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
#include <iterator>
#include <mutex>
#include <vector>

#include "compio/compio_file.hpp"

#ifdef _WIN32
#include <io.h>   // _chsize_s, _fileno
#else
#include <unistd.h> // ftruncate, fileno
#endif

#ifdef __linux__
#include <fcntl.h>        // fallocate
#include <linux/falloc.h> // FALLOC_FL_PUNCH_HOLE
#include <sys/stat.h>     // fstat
#endif

#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"
#include "compio/file.hpp"
#include "compio/sha256.hpp"
#include "compio/utils.hpp"

namespace compio {

free_blocks_manager::free_blocks_manager(const uint64_t *file_size)
    : total_free_(0),
      file_size_(file_size),
      cached_fragmentation_(0),
      fragmentation_dirty_(true) {
    assert(file_size_ != nullptr);
}

free_blocks_manager::region_map::iterator
free_blocks_manager::insert_region(region_map::iterator hint, uint64_t offset, uint64_t size) {
    by_size_.emplace(size, offset);
    return by_offset_.emplace_hint(hint, offset, size);
}

free_blocks_manager::region_map::iterator free_blocks_manager::erase_region(region_map::iterator it) {
    by_size_.erase({it->second, it->first});
    return by_offset_.erase(it);
}

void free_blocks_manager::clear_regions() {
    by_offset_.clear();
    by_size_.clear();
    next_fit_cursor_ = 0;
    total_free_ = 0;
    fragmentation_dirty_ = true;
}

bool free_blocks_manager::add_free_block(uint64_t offset, uint64_t size, uint64_t *merged_offset,
                                         uint64_t *merged_size) {
    if (size == 0)
        return false;
    if (offset > UINT64_MAX - size) // overflow guard: offset + size would wrap
        return false;

    auto next = by_offset_.lower_bound(offset);
    auto prev = next == by_offset_.begin() ? by_offset_.end() : std::prev(next);

    // A region that is already (partly) free must not be added again: the two
    // copies would be handed out to two different owners.
    if (next != by_offset_.end() && next->first < offset + size)
        return false;
    if (prev != by_offset_.end() && prev->first + prev->second > offset)
        return false;

    uint64_t start = offset;
    uint64_t end = offset + size;
    if (prev != by_offset_.end() && prev->first + prev->second == offset) {
        start = prev->first;
        erase_region(prev);
    }
    if (next != by_offset_.end() && next->first == end) {
        end = next->first + next->second;
        next = erase_region(next);
    }
    insert_region(next, start, end - start);

    if (merged_offset && merged_size) {
        *merged_offset = start;
        *merged_size = end - start;
    }

    total_free_ += size;
    fragmentation_dirty_ = true;
    return true;
}

uint64_t free_blocks_manager::allocate_block(uint64_t size, allocation_strategy strategy) {
    if (size == 0 || by_offset_.empty())
        return UINT64_MAX;

    auto target = by_offset_.end();

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

    if (target == by_offset_.end())
        return UINT64_MAX;

    // The request takes the start of the region; what is left stays free.
    const uint64_t allocated_offset = target->first;
    const uint64_t remaining_size = target->second - size;
    auto hint = erase_region(target);
    if (remaining_size != 0) {
        insert_region(hint, allocated_offset + size, remaining_size);
    }

    next_fit_cursor_ = allocated_offset + size;
    total_free_ -= size;
    fragmentation_dirty_ = true;

    return allocated_offset;
}

void free_blocks_manager::defragment() {
    // Regions are merged when they are freed, so this pass normally finds
    // nothing; it is kept as the documented way to restore that invariant.
    for (auto it = by_offset_.begin(); it != by_offset_.end();) {
        auto next = std::next(it);
        if (next != by_offset_.end() && it->first + it->second == next->first) {
            const uint64_t offset = it->first;
            const uint64_t size = it->second + next->second;
            erase_region(next);
            it = insert_region(erase_region(it), offset, size);
        } else {
            it = next;
        }
    }

    next_fit_cursor_ = 0;
    fragmentation_dirty_ = true;
}

void free_blocks_manager::print_list() const {
    for (const auto &[offset, size] : by_offset_) {
        DEBUG_PRINT("Block: %" PRIu64 ", %" PRIu64 "\n", offset, size);
        UNUSED(offset);
        UNUSED(size);
    }
}

uint8_t free_blocks_manager::get_cached_fragmentation() const {
    // The value depends on the container size as well, which changes without
    // touching the free list, so it is recomputed on every call (it is O(1)).
    cached_fragmentation_ = calculate_fragmentation();
    fragmentation_dirty_ = false;
    return cached_fragmentation_;
}

free_blocks_manager::fragmentation_stats free_blocks_manager::get_fragmentation_stats() const {
    fragmentation_stats stats = {};

    if (by_offset_.empty()) {
        return stats;
    }

    const size_t block_count = by_offset_.size();
    stats.num_free_regions = block_count;
    stats.total_free_bytes = total_free_;
    stats.largest_free_region = by_size_.rbegin()->first;
    stats.smallest_free_region = by_size_.begin()->first;
    stats.avg_free_region_size = static_cast<double>(total_free_) / static_cast<double>(block_count);
    stats.fragmentation_percent = calculate_fragmentation();

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

    auto it = by_offset_.upper_bound(offset);
    if (it == by_offset_.begin())
        return false;
    --it;
    return offset + size <= it->first + it->second;
}

uint8_t free_blocks_manager::calculate_fragmentation() const {
    // External fragmentation as the share of the container taken by free
    // regions: the space a compaction would give back. How scattered the free
    // space is does not matter here, only how much of it there is.
    const uint64_t container_size = file_size_ ? *file_size_ : 0;
    if (container_size == 0 || total_free_ == 0) {
        return 0;
    }
    const double share = static_cast<double>(total_free_) / static_cast<double>(container_size);
    return static_cast<uint8_t>(std::min(100.0, share * 100.0));
}

// On-disk layout of the allocator state, all fields little-endian:
//   count u64, count x (offset u64, size u64),
//   spare slot offset u64, spare slot size u64, crc32c u32 over the bytes before it.
// States written before the trailer was introduced end right after the pairs.
static constexpr size_t STATE_ENTRY_SIZE = 2 * sizeof(uint64_t);
static constexpr size_t STATE_TRAILER_SIZE = 2 * sizeof(uint64_t) + sizeof(uint32_t);
static constexpr uint64_t MIN_STATE_SLOT_SIZE = 64;

static void put_le64(uint8_t *dst, uint64_t v) {
    if (is_big_endian()) swap_uint64(&v);
    memcpy(dst, &v, sizeof(v));
}

static uint64_t get_le64(const uint8_t *src) {
    uint64_t v;
    memcpy(&v, src, sizeof(v));
    if (is_big_endian()) swap_uint64(&v);
    return v;
}

size_t free_blocks_manager::free_region_count() const { return by_offset_.size(); }

uint32_t free_blocks_manager::serialize(std::vector<uint8_t> &buffer) {
    const size_t block_count = free_region_count();
    const size_t list_size = sizeof(uint64_t) + block_count * STATE_ENTRY_SIZE;
    const uint32_t size_needed = static_cast<uint32_t>(list_size + STATE_TRAILER_SIZE);

    buffer.resize(size_needed);

    uint8_t *out = buffer.data();
    put_le64(out, block_count);
    out += sizeof(uint64_t);
    for (const auto &[offset, size] : by_offset_) {
        put_le64(out, offset);
        put_le64(out + sizeof(uint64_t), size);
        out += STATE_ENTRY_SIZE;
    }
    put_le64(out, spare_slot_offset_);
    put_le64(out + sizeof(uint64_t), spare_slot_size_);
    out += 2 * sizeof(uint64_t);

    uint32_t checksum = crc32c(buffer.data(), static_cast<size_t>(out - buffer.data()));
    if (is_big_endian()) swap_uint32(&checksum);
    memcpy(out, &checksum, sizeof(checksum));

    return size_needed;
}

bool free_blocks_manager::deserialize(const uint8_t *buffer, uint32_t size) {
    if (size < sizeof(uint64_t)) {
        return false;
    }

    const uint64_t count = get_le64(buffer);

    // Guard against integer overflow: count * 16 could wrap
    if (count > (UINT32_MAX - sizeof(uint64_t) - STATE_TRAILER_SIZE) / STATE_ENTRY_SIZE) {
        return false;
    }
    const uint32_t list_size = static_cast<uint32_t>(sizeof(uint64_t) + count * STATE_ENTRY_SIZE);
    if (size < list_size) {
        return false;
    }

    // A legacy state is exactly list_size long; anything longer carries the trailer.
    uint64_t spare_offset = 0;
    uint64_t spare_size = 0;
    if (size != list_size) {
        if (size < list_size + STATE_TRAILER_SIZE) {
            return false;
        }
        const uint8_t *trailer = buffer + list_size;
        uint32_t stored;
        memcpy(&stored, trailer + 2 * sizeof(uint64_t), sizeof(stored));
        if (is_big_endian()) swap_uint32(&stored);
        if (stored != crc32c(buffer, list_size + 2 * sizeof(uint64_t))) {
            return false;
        }
        spare_offset = get_le64(trailer);
        spare_size = get_le64(trailer + sizeof(uint64_t));
    }

    clear_regions();
    spare_slot_offset_ = spare_offset;
    spare_slot_size_ = spare_size;

    const uint8_t *in = buffer + sizeof(uint64_t);
    for (uint64_t i = 0; i < count; i++, in += STATE_ENTRY_SIZE) {
        const uint64_t offset = get_le64(in);
        const uint64_t block_size = get_le64(in + sizeof(uint64_t));

        // Skip invalid entries
        if (block_size == 0) continue;
        if (offset + block_size < offset) continue; // overflow
        if (file_size_ && offset + block_size > *file_size_) continue;

        if (!add_free_block(offset, block_size)) {
            // Overlapping free regions: the state is corrupted, start empty.
            clear_regions();
            spare_slot_offset_ = spare_slot_size_ = 0;
            return false;
        }
    }

    return true;
}

bool free_blocks_manager::save_to_file(compio_archive *archive) {
    if (!archive || !archive->file || !archive->header) {
        return false;
    }

    const uint64_t reserved_end = readonly(archive->header, header)->reserved_size();
    const uint64_t logical_end = readonly(archive->header, header)->file_size;
    auto is_slot = [&](uint64_t offset, uint64_t size) {
        return offset >= reserved_end && size != 0 && size <= UINT64_MAX - offset &&
               offset + size <= logical_end;
    };

    uint64_t current_offset = readonly(archive->header, header)->allocator_state_offset;
    uint64_t current_size = readonly(archive->header, header)->allocator_state_size;
    if (!is_slot(current_offset, current_size)) {
        current_offset = current_size = 0;
    }
    if (!is_slot(spare_slot_offset_, spare_slot_size_)) {
        spare_slot_offset_ = spare_slot_size_ = 0;
    }

    // Sized for the list as it will be if the spare slot has to be released
    // below, which adds at most one entry.
    const uint64_t needed = sizeof(uint64_t) +
                            (static_cast<uint64_t>(free_region_count()) + 1) * STATE_ENTRY_SIZE +
                            STATE_TRAILER_SIZE;
    if (needed > UINT32_MAX / 2) {
        WARNING_PRINT("warning: allocator state is too large to save\n");
        return false;
    }

    // The state alternates between two slots, like the double-buffered header:
    // the durable header references the current slot until the next header is
    // published, so the new copy goes to the spare one. Both slots stay
    // allocated, which keeps repeated saves from growing the archive.
    uint64_t pos = spare_slot_offset_;
    uint64_t slot_size = spare_slot_size_;
    bool appended = false;
    if (slot_size < needed) {
        if (slot_size != 0) {
            add_free_block(spare_slot_offset_, spare_slot_size_);
        }
        slot_size = MIN_STATE_SLOT_SIZE;
        while (slot_size < needed) {
            slot_size *= 2;
        }
        pos = std::max(logical_end, reserved_end);
        appended = true;
    }
    spare_slot_offset_ = current_offset;
    spare_slot_size_ = current_size;

    std::vector<uint8_t> buffer;
    serialize(buffer);
    assert(buffer.size() <= slot_size);
    buffer.resize(slot_size, 0);
    const uint32_t size = static_cast<uint32_t>(slot_size);

    bool in_batch = archive->wal && archive->wal->get_batch_depth() > 0;
    
    if (!in_batch && archive->wal) {
        archive->wal->begin_transaction();
    }
    
    if (archive->wal) {
        if (!archive->wal->log_write(WalRecordType::ALLOCATOR, pos, buffer.data(), size)) {
            WARNING_PRINT("warning: WAL log_write failed in allocator.save_state\n");
            if (!in_batch) {
                archive->wal->rollback_transaction();
            }
            return false;
        }
    }

    if (fseek64(archive->file, static_cast<int64_t>(pos), SEEK_SET) != 0) {
        WARNING_PRINT("warning: fseek returned error in allocator.save_state\n");
        if (!in_batch && archive->wal) {
            archive->wal->rollback_transaction();
        }
        return false;
    }

    // Write to archive file (buffered) - after WAL commit (Write-Ahead)
    DEBUG_PRINT("[W][allocator]addr=%" PRIu64 ";size=%" PRIu32 "\n", pos, size);
    size_t written = fwrite(buffer.data(), 1, size, archive->file);
    if (written != size) {
        WARNING_PRINT(
            "warning: fwrite failed to write all bytes in allocator.save_state (%zu < %u)\n", written, size);
        if (!in_batch && archive->wal) {
            archive->wal->rollback_transaction();
        }
        return false;
    }
    archive->header->allocator_state_offset = pos;
    archive->header->allocator_state_size = size;
    if (appended) {
        archive->header->file_size = pos + size;
    }

    fflush(archive->file);

    if (!in_batch && archive->wal) {
        if (!archive->wal->commit_transaction(archive->file, archive->config.wal_max_size_bytes)) {
            WARNING_PRINT("warning: WAL commit failed in allocator.save_state\n");
            return false;
        }
    }

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

free_blocks_manager::region_map::iterator free_blocks_manager::find_first_fit(const uint64_t size) {
    // Nothing fits: skip the walk over every region.
    if (by_size_.rbegin()->first < size)
        return by_offset_.end();
    return std::find_if(by_offset_.begin(), by_offset_.end(),
                        [size](const auto &region) { return region.second >= size; });
}

free_blocks_manager::region_map::iterator free_blocks_manager::find_best_fit(const uint64_t size) {
    auto it = by_size_.lower_bound({size, 0});
    return it == by_size_.end() ? by_offset_.end() : by_offset_.find(it->second);
}

free_blocks_manager::region_map::iterator free_blocks_manager::find_worst_fit(const uint64_t size) {
    const uint64_t largest = by_size_.rbegin()->first;
    if (largest < size)
        return by_offset_.end();
    return by_offset_.find(by_size_.lower_bound({largest, 0})->second);
}

free_blocks_manager::region_map::iterator free_blocks_manager::find_next_fit(const uint64_t size) {
    if (by_size_.rbegin()->first < size)
        return by_offset_.end();

    const auto fits = [size](const auto &region) { return region.second >= size; };
    const auto start = by_offset_.lower_bound(next_fit_cursor_);
    auto it = std::find_if(start, by_offset_.end(), fits);
    if (it == by_offset_.end()) {
        it = std::find_if(by_offset_.begin(), start, fits);
        if (it == start)
            return by_offset_.end();
    }
    return it;
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
    std::shared_lock<std::shared_mutex> lock(archive_->allocator_mutex);
    return blocks_manager_.get_cached_fragmentation();
}

free_blocks_manager::fragmentation_stats block_allocator::get_fragmentation_stats() const {
    if (!archive_) return {};
    std::shared_lock<std::shared_mutex> lock(archive_->allocator_mutex);
    return blocks_manager_.get_fragmentation_stats();
}

uint64_t block_allocator::allocate(uint64_t size) {
    if (size == 0) {
        return UINT64_MAX;
    }

    std::unique_lock<std::shared_mutex> lock(archive_->allocator_mutex);

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

void block_allocator::deallocate(uint64_t offset, uint64_t size) {
    deallocate(offset, size, true);
}

void block_allocator::deallocate(uint64_t offset, uint64_t size, bool perform_maintenance) {
    if (offset == UINT64_MAX || size == 0 || !archive_ || !archive_->header) {
        return;
    }

    bool run_maintenance = false;
    {
        std::unique_lock<std::shared_mutex> lock(archive_->allocator_mutex);

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

    uint64_t merged_offset = offset;
    uint64_t merged_size = size;
    if (!blocks_manager_.add_free_block(offset, size, &merged_offset, &merged_size)) {
        return;
    }
    if (wal_) {
             // Similar to allocate, strict WAL logging of every free list change is expensive.
             // We rely on periodic checkpoints of the allocator state.
    }

    if (archive_->config.fill_holes_with_zeros && archive_->file) {
        release_to_filesystem(offset, size, merged_offset, merged_size);
    }

    // Check if defragmentation is needed (throttled to avoid O(N) cost on every dealloc)
    if (perform_maintenance && maintenance_suspended_ == 0 && ++deallocate_count_ % 64 == 0) {
        run_maintenance = true;
    }
    }

    if (run_maintenance) {
        maintenance();
    }
}

bool block_allocator::punch_hole(uint64_t offset, uint64_t size, uint64_t merged_offset,
                                 uint64_t merged_size) {
#ifdef __linux__
    const int fd = fileno(archive_->file);
    if (fs_block_size_ == 0) {
        struct stat st;
        fs_block_size_ = (fstat(fd, &st) == 0 && st.st_blksize > 0)
                             ? static_cast<uint64_t>(st.st_blksize) : 4096;
    }
    const uint64_t bs = fs_block_size_;
    const uint64_t end = offset + size;
    const uint64_t merged_end = merged_offset + merged_size;

    // The filesystem can only take back whole blocks. A freed region seldom
    // covers one on its own, but together with the free neighbours it has just
    // been merged with it may, so the range is widened to the block boundaries
    // that lie inside the merged free region.
    const uint64_t lo = std::max((merged_offset + bs - 1) / bs * bs, offset / bs * bs);
    const uint64_t hi = std::min(merged_end / bs * bs, (end + bs - 1) / bs * bs);
    const uint64_t punch_start = std::min(lo, offset);
    const uint64_t punch_end = std::max(hi, end);

    return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                     static_cast<off_t>(punch_start),
                     static_cast<off_t>(punch_end - punch_start)) == 0;
#else
    UNUSED(offset);
    UNUSED(size);
    UNUSED(merged_offset);
    UNUSED(merged_size);
    return false;
#endif
}

void block_allocator::release_to_filesystem(uint64_t offset, uint64_t size, uint64_t merged_offset,
                                            uint64_t merged_size) {
    DEBUG_PRINT("[W][deallocate]addr=%" PRIu64 ";size=%" PRIu64 "\n", offset, size);

    // Buffered writes into the region must reach the file first, otherwise they
    // would land after the hole is punched and bring the blocks back.
    fflush(archive_->file);

    if (hole_punching_supported_) {
        if (punch_hole(offset, size, merged_offset, merged_size)) {
            return;
        }
        hole_punching_supported_ = false;
    }

    // No hole punching on this platform or filesystem: overwrite with zeros,
    // which at least lets a compressing filesystem reclaim the space.
    static constexpr size_t BUFFER_SIZE = 4096;
    static uint8_t zeros[BUFFER_SIZE] = {0};

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

void block_allocator::force_defragmentation() {
    // Flush caches before locking allocator/index to avoid deadlock.
    // block::~block() calls allocate/deallocate/update which lock allocator_mutex and index->mutex.
    if (archive_->file) {
        archive_->block_reader->set_maintenance_mode(true);
        archive_->block_reader->clear_cache();
        // Use public clear_cache which handles its own locking if needed, 
        // or just relies on the fact we are the only thread (compio_defragment holds archive mutex).
        archive_->index->clear_cache();
    }

    auto index_lock = archive_->index->get_lock();
    std::unique_lock<std::shared_mutex> alloc_lock(archive_->allocator_mutex);

    blocks_manager_.defragment();
    if (archive_->file && archive_->index) {
        perform_defragmentation(true);
        compaction_floor_ = blocks_manager_.get_cached_fragmentation();
    }
    last_fragmentation_ = blocks_manager_.get_cached_fragmentation();

    if (archive_->file) {
        archive_->block_reader->set_maintenance_mode(false);
    }
}

void block_allocator::maintenance(bool at_checkpoint) {
    if (maintenance_suspended_ > 0) {
        return;
    }

    uint8_t current_fragmentation = get_fragmentation();
    uint8_t threshold = archive_->config.fragmentation_threshold;

    // Free space that the last compaction could not remove (regions pinned in
    // front of index nodes or of the files table) must not trigger another full
    // pass on its own. A checkpoint compaction can also move the files table, so
    // it is always worth a try.
    if (current_fragmentation < compaction_floor_) {
        compaction_floor_ = current_fragmentation;
    }

    if (current_fragmentation > threshold &&
        (at_checkpoint || current_fragmentation > compaction_floor_)) {
        auto index_lock = archive_->index->try_get_lock();
        if (!index_lock.owns_lock()) {
             // If we can't lock the index, we skip defragmentation for now.
             // This avoids deadlocks when called from within a B-tree operation (which holds the lock).
             return;
        }

        // Flush caches before locking allocator to avoid deadlock.
        // block::~block() calls allocate/deallocate which lock allocator_mutex.
        if (archive_->file) {
            archive_->block_reader->set_maintenance_mode(true);
            archive_->block_reader->clear_cache();
            archive_->index->_clear_cache();
        }

        std::unique_lock<std::shared_mutex> alloc_lock(archive_->allocator_mutex);

        if (maintenance_suspended_ > 0) {
            if (archive_->file) {
                archive_->block_reader->set_maintenance_mode(false);
            }
            return;
        }

        if (blocks_manager_.get_cached_fragmentation() > threshold) {
            blocks_manager_.defragment();

            if (blocks_manager_.get_cached_fragmentation() > threshold) {
                if (archive_->file && archive_->index) {
                    perform_defragmentation(at_checkpoint);
                    compaction_floor_ = blocks_manager_.get_cached_fragmentation();
                }
            }
        }
        last_fragmentation_ = blocks_manager_.get_cached_fragmentation();

        if (archive_->file) {
            archive_->block_reader->set_maintenance_mode(false);
        }
    } else {
        std::unique_lock<std::shared_mutex> lock(archive_->allocator_mutex);
        last_fragmentation_ = blocks_manager_.get_cached_fragmentation();
    }
}

bool block_allocator::save_state(compio_archive *archive) {
    if (!archive) {
        return false;
    }
    std::unique_lock<std::shared_mutex> alloc_lock(archive->allocator_mutex);
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
    std::unique_lock<std::shared_mutex> alloc_lock(archive->allocator_mutex);
    std::lock_guard<std::mutex> head_lock(archive->header_mutex);
    std::lock_guard<std::mutex> io_lock(archive->io_mutex);
    return blocks_manager_.load_from_file(archive);
}

// Private methods

bool block_allocator::needs_defragmentation() const {
    return blocks_manager_.get_cached_fragmentation() > archive_->config.fragmentation_threshold;
}

static bool sync_file(FILE *file) {
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

void block_allocator::perform_defragmentation(bool relocate_files_table) {
    // Flush all cached/dirty blocks to disk first so that every index entry
    // has a real physical address before we start moving data.
    // NOTE: This must be done by the caller (maintenance/force_defragmentation)
    // BEFORE holding allocator_mutex to avoid deadlocks, as clear_cache() triggers
    // block destructors which call allocate/deallocate.

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

    // v5 specific: The files table is stored as a reserved block in the archive.
    // We must treat it as an obstacle during defragmentation to avoid overwriting it.
    uint64_t ft_addr = 0;
    uint64_t ft_size = 0;
    {
        const auto &hdr = readonly(archive_->header, header);
        if (hdr->magic_number == COMPIO_MAGIC_NUMBER) {
             ft_addr = hdr->files_table_addr;
             // Must match the on-disk entry size used by files_table::write_to and
             // sync_files_table: name + size(u64) + file_id(u64). Undercounting here
             // makes the truncation guard below slice the tail off a files table that
             // sits after the data, corrupting both header copies on reopen.
             ft_size = (uint64_t)hdr->files_table_capacity *
                       (COMPIO_FNAME_MAX_SIZE + 2 * sizeof(uint64_t));
        }
    }

    // Collect final positions of placed storage blocks for gap computation.
    std::vector<std::pair<uint64_t, uint64_t>> placed_blocks;

    for (auto &[key, val] : used_blocks) {
        const uint64_t src = val.addr;

        if (src < readonly(archive_->header, header)->disk_size()) {
            continue;
        }

        // Read signature + compressed size from on-disk metadata. The signature
        // determines the metadata footprint (v1=22, v2=38 with {hash,pos} back-ref).
        uint64_t compressed_size = 0;
        uint8_t sig = 0;
        {
            if (fseek64(archive_->file, static_cast<int64_t>(src), SEEK_SET) != 0 ||
                fread(&sig, 1, 1, archive_->file) != 1) {
                WARNING_PRINT("warning: perform_defragmentation: failed to read signature at %" PRIu64 "\n", src);
                continue;
            }
            const int64_t meta_offset = static_cast<int64_t>(src) + 2;
            if (fseek64(archive_->file, meta_offset, SEEK_SET) != 0 ||
                lendian_fread(&compressed_size, sizeof(compressed_size), 1, archive_->file) != 1 ||
                compressed_size == 0) {
                WARNING_PRINT("warning: perform_defragmentation: failed to read metadata at %" PRIu64 "\n", src);
                continue;
            }
        }
        const uint64_t meta_size = storage_block::meta_size_for(sig);
        if (meta_size == 0) {
            WARNING_PRINT("warning: perform_defragmentation: bad signature %u at %" PRIu64 "\n", sig, src);
            continue;
        }
        const uint64_t block_size = meta_size + compressed_size;

        // Ensure write_pos doesn't overlap with ANY reserved region (Files Table or B-tree nodes)
        while (true) {
            bool collision = false;
            
            // 1. Check files table (v5)
            if (ft_addr != 0) {
                 // Check intersection [write_pos, write_pos + block_size) AND [ft_addr, ft_addr + ft_size)
                 if (write_pos < ft_addr + ft_size && write_pos + block_size > ft_addr) {
                     // Collision! Skip past the files table.
                     write_pos = ft_addr + ft_size;
                     collision = true;
                 }
            }
            
            // 2. Check B-tree nodes
            if (!collision && overlaps_btree_node(write_pos, block_size)) {
                auto it = std::lower_bound(node_addrs.begin(), node_addrs.end(), write_pos);
                if (it != node_addrs.begin()) {
                    auto prev = std::prev(it);
                    if (*prev + btree_node_size > write_pos) {
                        write_pos = *prev + btree_node_size;
                        collision = true;
                    }
                }
                if (!collision && it != node_addrs.end() && *it < write_pos + block_size) {
                    write_pos = *it + btree_node_size;
                    collision = true;
                }
            }
            
            if (!collision) break;
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

    if (fflush(archive_->file) != 0 || !sync_file(archive_->file)) {
        WARNING_PRINT("warning: perform_defragmentation: failed to sync moved blocks\n");
        return;
    }

    // The files table is an obstacle while blocks are being moved, so a table
    // that sits behind the data keeps a hole in front of it. Move it down to the
    // first position after the compacted blocks. This is only done when the
    // caller publishes a header right away: the durable header must never
    // reference a table copy that later writes can reuse.
    if (relocate_files_table && ft_addr != 0 && ft_size != 0) {
        uint64_t table_pos = write_pos;
        while (overlaps_btree_node(table_pos, ft_size)) {
            auto it = std::upper_bound(node_addrs.begin(), node_addrs.end(), table_pos);
            if (it != node_addrs.begin() && *std::prev(it) + btree_node_size > table_pos) {
                table_pos = *std::prev(it) + btree_node_size;
            } else {
                table_pos = *it + btree_node_size;
            }
        }

        auto write_table = [&](uint64_t pos) {
            readonly(archive_->header, header)->ftable.write_to(archive_->file, pos);
            return fflush(archive_->file) == 0 && sync_file(archive_->file);
        };

        if (table_pos < ft_addr) {
            bool movable = true;
            if (table_pos + ft_size > ft_addr) {
                // The target overlaps the copy the durable header references.
                // Park the table behind everything and make that copy durable
                // first, so the overlapping write below cannot lose the table.
                const uint64_t park_pos = readonly(archive_->header, header)->file_size;
                movable = write_table(park_pos);
                if (movable) {
                    archive_->header->files_table_addr = park_pos;
                    archive_->header->file_size = park_pos + ft_size;
                    archive_->header->allocator_state_offset = 0;
                    archive_->header->allocator_state_size = 0;
                    ft_addr = park_pos;
                    movable = archive_->publish_header && archive_->publish_header();
                }
            }
            if (movable && write_table(table_pos)) {
                archive_->header->files_table_addr = table_pos;
                ft_addr = table_pos;
            }
        }
    }

    // Compute the new end of the container: max of write_pos and end of last B-tree node.
    uint64_t truncate_pos = write_pos;
    if (!node_addrs.empty()) {
        uint64_t last_node_end = node_addrs.back() + btree_node_size;
        if (last_node_end > truncate_pos) {
            truncate_pos = last_node_end;
        }
    }
    
    // v5: The files table may be located after data/nodes.
    if (ft_addr != 0 && ft_addr + ft_size > truncate_pos) {
        truncate_pos = ft_addr + ft_size;
    }

    // The physical file is cut later, once a header describing the compacted
    // layout is durable: until then the previous header still references the
    // tail (files table, allocator state).
    archive_->truncate_after_publish = true;

    // Update logical file_size in header and rebuild free-block manager.
    archive_->header->file_size = truncate_pos;
    // The saved allocator state is not an obstacle for compaction, so its slots
    // are gone now; the next save starts with fresh ones.
    archive_->header->allocator_state_offset = 0;
    archive_->header->allocator_state_size = 0;

    blocks_manager_ = free_blocks_manager(&archive_->header->file_size);

    // Rebuild free blocks from gaps between all occupied regions (storage blocks + B-tree nodes).
    std::vector<std::pair<uint64_t, uint64_t>> occupied;
    occupied.reserve(placed_blocks.size() + node_addrs.size() + 1);
    for (auto &pb : placed_blocks) {
        occupied.push_back(pb);
    }
    for (uint64_t na : node_addrs) {
        if (na >= truncate_pos) break;
        occupied.push_back({na, btree_node_size});
    }
    
    // v5: Mark files_table as occupied so allocator doesn't hand it out as free space.
    if (ft_addr != 0) {
        occupied.push_back({ft_addr, ft_size});
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
    
    DEBUG_PRINT("[AL] perform_defragmentation complete. New file size: %" PRIu64 "\n", write_pos);
}

} // namespace compio
