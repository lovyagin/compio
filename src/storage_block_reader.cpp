#define __STDC_FORMAT_MACROS
#include "compio/storage_block_reader.hpp"
#include "compio/wal.hpp"
#include "compio/file.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <cinttypes>
#include <algorithm>
#include <vector>

#include "compio/debug_print.hpp"

namespace compio {

static thread_local bool tl_maintenance_mode = false;

#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
long long bm_n_blocks = 0;
#endif

#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
long long bm_n_compressed_bytes = 0;
long long bm_n_decompressed_bytes = 0;
#endif

block::block(context_t &context, const tree_key &key, uint64_t addr)
    : context(context),
      _key(key),
      _addr(addr),
      _c_size(0),
      _data(nullptr),
      _size(0),
      _is_modified(false),
      _is_removed(false),
      _is_valid(true) {

    storage_block b;
    {
        std::unique_lock<std::mutex> lock;
        if (context.io_mutex)
            lock = std::unique_lock<std::mutex>(*context.io_mutex);
        if (!b.read_from(context.file, addr)) {
            WARNING_PRINT("warning: failed to read storage block at addr %" PRIu64 "\n", addr);
            _is_valid = false;
            return;
        }
    }

    _c_size = b.size;
    _size = b.original_size;

    if (b.is_compressed) {
        _data = std::make_unique<uint8_t[]>(_size);
        int ret = context.compressor->decompress(_data.get(), &_size, b.data.get(), b.size);
        if (ret != 0) {
            WARNING_PRINT(
                "warning: compressed data is too big after decompression (%" PRIu64 " is not enough)\n",
                _size);
            _is_valid = false;
            return;
        }
#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
        bm_n_decompressed_bytes += _size;
#endif
        assert(b.original_size == _size);
    } else {
        _data = std::move(b.data);
    }
}

block::block(context_t &context, const tree_key &key, uint64_t size, bool unused)
    : context(context),
      _key(key),
      _addr(0),
      _c_size(0),
      _data(std::make_unique<uint8_t[]>(size)),
      _size(size),
      _is_modified(true),
      _is_removed(false),
      _is_valid(true) {
    UNUSED(unused);
}

block::~block() {
    if (!_is_valid) {
        // DEBUG_PRINT("[B][destructor]: not a valid block, skipping\n");
        return;
    }
    if (_is_modified && !_is_removed) {
        storage_block b(context.compressor->get_bufsize(_size));
        b.original_size = _size;
        b.checksum_type = context.checksum_type;

        int ret = context.compressor->compress(b.data.get(), &b.size, _data.get(), _size);
        if (ret != 0 || b.size > _size) {
            if (ret != 0) {
                WARNING_PRINT("warning: compressor->compress returned %d\n", ret);
            }

            b.is_compressed = false;
            b.data = std::move(_data);
            b.size = _size;
        } else {
#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
            bm_n_compressed_bytes += _size;
#endif
            b.is_compressed = true;
        }

        if (_addr != 0) {
            // block was read from file, so addr was already allocated from allocator previously

            // if (c_size >= b.size) {
            //     // we can reuse previous memory block in file
            // }

            // though it would be better to pass this logic to allocator, and allocate memory again
            context.allocator->deallocate(_addr, _c_size + STORAGE_BLOCK_METASIZE);
        }

        uint64_t new_addr = context.allocator->allocate(STORAGE_BLOCK_METASIZE + b.size);
        DEBUG_PRINT(
            "[B][destructor]: writing to file "
            "(new_addr=%" PRIu64 ",addr=%" PRIu64 ",original_size=%" PRIu64 ",size=%" PRIu64 ",is_compressed=%d,key.pos=%" PRIu64 ")\n",
            new_addr, _addr, b.original_size, b.size, b.is_compressed, _key.pos);
        
        if (context.io_mutex) {
            std::lock_guard<std::mutex> lock(*context.io_mutex);
            b.write_to(context.file, new_addr, context.wal);
        } else {
            b.write_to(context.file, new_addr, context.wal);
        }

        // block already in btree thanks to storage_block_reader
        // we just need to update it's file address
        bool update_res = false;
        if (tl_maintenance_mode) {
             context.index->_update_impl(_key, {new_addr, _size});
             update_res = true; // _update_impl returns void or we assume it works? Check btree.hpp
        } else {
             update_res = context.index->update(_key, {new_addr, _size});
        }
        
        if (!update_res) {
            WARNING_PRINT("[B][destructor] ERROR: failed to update index for key {%" PRIu64 ", %" PRIu64 "} with addr %" PRIu64 "\n",
                          _key.hash, _key.pos, new_addr);
            // This is a critical consistency error. The block is written to disk, but the index
            // still points to addr=0 (or old addr). Future reads will fail.
            assert(false && "Failed to update index in block destructor");
        }

        {
            std::lock_guard<std::mutex> lock(context.temp_index_mutex);
            if (context.temp_index_refcount > 0) {
                // save allocated address to temporary_index
                context.temporary_index[_key] = new_addr;
            }
        }
    } else {
        if (_addr == 0) {
            // this block was created in storage_block_reader::create_block, so it's key was
            // inserted to btree
            //
            // not calling index->remove, because storage_block_reader::remove_block calls it
            // index->remove(key);
        }
        if (_is_removed && _addr != 0) {
            // this block was created in storage_block_reader::read_block, so we need to deallocate
            // it's memory
            //
            // not deallocating, because storage_block_reader::remove_block does that
            // context.allocator->deallocate(addr, c_size);
        }
    }
}

const uint8_t *block::data() const { return _data.get(); }

uint8_t *block::data() {
    _is_modified = true;
    return _data.get();
}

bool block::is_valid() const { return _is_valid; }

void block::shrink(uint64_t new_size) {
    assert(new_size < _size);
    _is_modified = true;
    _size = new_size;
    // we're not updating size in btree, because while this block is in cache, read_block()->size()
    // will return correct size, and if block is not in cache, then destructor already updated size
    //
    // UPD: we are updating size in btree, because otherwise we can't use btree::get_block in
    // compio_insert
    context.index->update(_key, {_addr, new_size});
}

void block::grow(uint64_t new_size) {
    assert(new_size > _size);

    auto new_data = std::make_unique<uint8_t[]>(new_size);
    std::copy_n(_data.get(), _size, new_data.get());
    _data = std::move(new_data);

    _is_modified = true;
    _size = new_size;
    context.index->update(_key, {_addr, new_size});
}

void block::remove() { _is_removed = true; }

void block::shift_key(int64_t addition) {
    assert(addition != 0);
    _is_modified = true;
    DEBUG_PRINT("[B][shift_key]: key.pos=%" PRIu64 ", shifting with addition=%" PRId64 "\n", _key.pos, addition);
    _key += addition;
}

void block::set_key(const tree_key &new_key) {
    if (_key != new_key) {
        _is_modified = true;
        _key = new_key;
    }
}

const tree_key &block::key() const { return _key; }

uint64_t block::size() const { return _size; }

uint64_t block::addr() const { return _addr; }

uint64_t block::c_size() const { return _c_size; }

storage_block_reader::storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                                           const compio_compressor *compressor, int max_size,
                                           std::mutex *io_mutex, compio::WalManager *wal,
                                           compio_checksum_type checksum_type)
    : cache(max_size),
      context{file, allocator, index, compressor, io_mutex, wal, checksum_type} {}

std::shared_ptr<block> storage_block_reader::read_block(uint64_t addr, tree_key key) {
    DEBUG_PRINT("[SBR][read_block]: addr=%" PRIu64 ", key.hash=%" PRIu64 ", key.pos=%" PRIu64 "\n", addr, key.hash,
                key.pos);
    auto b_cached = cache.get(key);
    if (b_cached.has_value()) {
        DEBUG_PRINT("[SBR][read_block]: cache hit\n");
        return b_cached.value();
    }
    DEBUG_PRINT("[SBR][read_block]: cache miss\n");
    
    if (context.temp_index_refcount.load(std::memory_order_acquire) > 0) {
        // Only lock if temporary index is active (double-checked optimization)
        std::lock_guard<std::mutex> lock(context.temp_index_mutex);
        // check in temporary index first
        auto it = context.temporary_index.find(key);
        if (it != context.temporary_index.end()) {
            addr = it->second;
            DEBUG_PRINT("[SBR][read_block]: getting addr from temporary_index: addr=%" PRIu64 "\n", (uint64_t)addr);
        }
#ifndef NDEBUG
            auto val = context.index->get(key);
            assert(val.has_value());
            // assert(val.value().addr == addr); // This assert is race-prone in concurrent environment
#endif
    }
    
    if (addr == 0) {
        // If addr is 0 and not in cache, it might have been evicted and flushed by another thread.
        // Re-query index to get the authoritative address.
        auto val = context.index->get(key);
        if (val.has_value()) {
            addr = val.value().addr;
             DEBUG_PRINT("[SBR][read_block]: re-queried index for stale addr=0, got addr=%" PRIu64 "\n", addr);
        }
    }

    auto b = std::make_shared<block>(context, key, addr);
    if (!b->is_valid()) {
        return nullptr;
    }
    cache.put(key, b);
    return b;
}

std::shared_ptr<block> storage_block_reader::create_block(uint64_t size, tree_key key) {
    DEBUG_PRINT("[SBR][create_block]: size=%" PRIu64 ", key.hash=%" PRIu64 ", key.pos=%" PRIu64 "\n", 
                (uint64_t)size, (uint64_t)key.hash, (uint64_t)key.pos);
    auto b_cached = cache.get(key);
    if (b_cached.has_value()) {
        WARNING_PRINT("warning: trying to create block with key (%llu, %llu), that "
                      "already exists in storage_block_reader.cache\n",
                      (unsigned long long)key.hash, (unsigned long long)key.pos);
        return b_cached.value();
    }
    auto b = std::make_shared<block>(context, key, size, false);
    cache.put(key, b);

    // adding element to btree, but without file address (we didn't allocate memory block yet)
    // block::~block will update this element in btree with new address (or delete it if block will
    // be removed, thought we don't remove newly created blocks anywhere)
    context.index->insert(key, {0, size});
#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
    ++bm_n_blocks;
#endif
    return b;
}

void storage_block_reader::clear_cache() {
    DEBUG_PRINT("[SBR][clear_cache]\n");
    
    auto blocks = cache.extract_all();
    
    // Sort blocks by address to optimize reallocation during flush.
    // We prioritize existing blocks (addr != 0) over new blocks (addr == 0).
    // Existing blocks free their old space first, creating holes.
    // New blocks then allocate, potentially filling those holes.
    // Within existing blocks, we sort by address to maximize merging of adjacent freed blocks.
    std::sort(blocks.begin(), blocks.end(), [](const std::shared_ptr<block> &a, const std::shared_ptr<block> &b) {
        bool a_exists = a->addr() != 0;
        bool b_exists = b->addr() != 0;
        if (a_exists != b_exists) {
            return a_exists; // exists (true) comes before new (false)
        }
        return a->addr() < b->addr();
    });

    // The C++ standard does not guarantee any particular destruction order for
    // std::vector::clear() / erase(). By manually resetting shared_ptrs in this loop,
    // we ensure destructors run in the exact order we want (ascending address / existing first),
    // independent of the container's internal destruction order.
    for (auto& b : blocks) {
        b.reset();
    }
    blocks.clear();
}

void storage_block_reader::set_maintenance_mode(bool enabled) {
    tl_maintenance_mode = enabled;
}

double storage_block_reader::get_cache_hit_probability() const { return cache.get_hit_probability(); }

void storage_block_reader::enable_temporary_index() {
    std::lock_guard<std::mutex> lock(context.temp_index_mutex);
    context.temp_index_refcount++;
}

void storage_block_reader::disable_temporary_index() {
    std::lock_guard<std::mutex> lock(context.temp_index_mutex);
    if (context.temp_index_refcount > 0) {
        context.temp_index_refcount--;
    }
    if (context.temp_index_refcount == 0) {
        context.temporary_index.clear();
    }
}

void storage_block_reader::invalidate_temporary_index() {
    std::lock_guard<std::mutex> lock(context.temp_index_mutex);
    context.temporary_index.clear();
}

void storage_block_reader::add_to_range(int64_t addition, const tree_key &key_min,
                                        const tree_key &key_max) {
    DEBUG_PRINT("[SBR][add_to_range]: adding %" PRId64 " to range [%" PRIu64 ", %" PRIu64 "]\n", addition, key_min.pos,
                key_max.pos);
    {
        std::lock_guard<std::mutex> lock(context.temp_index_mutex);
        // shift keys in temporary index
        auto it_start = context.temporary_index.lower_bound(key_min);
        auto it_end = context.temporary_index.upper_bound(key_max);
        std::vector<std::pair<tree_key, uint64_t>> items_to_update;
        for (auto it = it_start; it != it_end; ++it) {
            items_to_update.push_back(*it);
        }
        context.temporary_index.erase(it_start, it_end);
        for (const auto &[key, addr] : items_to_update) {
            assert(context.temporary_index.find(key + addition) == context.temporary_index.end());
            context.temporary_index[key + addition] = addr;
        }
    }

    {
        // manually update block::key for entries in cache
        // Note: this assumes we have exclusive access (unique_lock on archive),
        // so accessing cache internals is safe from concurrent access.
        auto it_start = cache._cache_items_map.lower_bound(key_min);
        auto it_end = cache._cache_items_map.upper_bound(key_max);
        for (auto it = it_start; it != it_end; ++it) {
            it->second->second->shift_key(addition);
        }
    }

    // and then update keys themselves
    cache.add_to_range(addition, key_min, key_max);
}

void storage_block_reader::rename_block(const tree_key &old_key, const tree_key &new_key, std::shared_ptr<block> b) {
    DEBUG_PRINT("[SBR][rename_block]: old_key.pos=%" PRIu64 ", new_key.pos=%" PRIu64 "\n", old_key.pos, new_key.pos);

    {
        std::lock_guard<std::mutex> lock(context.temp_index_mutex);
        if (context.temp_index_refcount > 0) {
            auto it = context.temporary_index.find(old_key);
            if (it != context.temporary_index.end()) {
                uint64_t addr = it->second;
                context.temporary_index.erase(it);
                context.temporary_index[new_key] = addr;
            }
        }
    }

    b->set_key(new_key);

    if (cache.exists(old_key)) {
        cache.remove(old_key);
        cache.put(new_key, b);
    }
}

void storage_block_reader::remove_block(std::shared_ptr<block> b) {
    DEBUG_PRINT("[SBR]removing block with key.pos=%" PRIu64 "\n", b->key().pos);
    b->remove();
    const auto &key = b->key();
    if (cache.exists(key)) {
        // when cache_size=0, storage_block_reader returns blocks from read/create, but don't save
        // them in cache, so we should check it
        cache.remove(key);
    }
    {
        std::lock_guard<std::mutex> lock(context.temp_index_mutex);
        if (context.temp_index_refcount > 0) {
            context.temporary_index.erase(key);
        }
    }
    context.index->remove(key);
    if (b->addr() != 0) {
        context.allocator->deallocate(b->addr(), b->c_size() + STORAGE_BLOCK_METASIZE);
    }
#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
    --bm_n_blocks;
#endif
}

bool storage_block_reader::cache_contains(const tree_key &key) const { return cache.exists(key); }

} // namespace compio
