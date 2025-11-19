#include "compio/storage_block_reader.hpp"

#include <cassert>

#include "compio/debug_print.hpp"

namespace compio {

block::block(context_t &context, const tree_key &key, uint64_t addr)
    : context(context),
      key(key),
      addr(addr),
      c_size(0),
      dec_data(nullptr),
      dec_size(0),
      is_modified(false),
      is_removed(false),
      is_valid(true) {

    storage_block b;
    b.read_from(context.file, addr);

    c_size = b.size;
    dec_size = b.original_size;

    if (b.is_compressed) {
        dec_data = std::make_unique<uint8_t[]>(dec_size);
        int ret = context.compressor->decompress(dec_data.get(), &dec_size, b.data.get(), b.size);
        if (ret != 0) {
            WARNING_PRINT(
                "warning: compressed data is too big after decompression (%lu is not enough)\n",
                dec_size);
            is_valid = false;
            return;
        }
        assert(b.original_size == dec_size);
    } else {
        dec_data = std::move(b.data);
    }
}

block::block(context_t &context, const tree_key &key, uint64_t size, bool unused)
    : context(context),
      key(key),
      addr(0),
      c_size(0),
      dec_data(std::make_unique<uint8_t[]>(size)),
      dec_size(size),
      is_modified(true),
      is_removed(false),
      is_valid(true) {
    UNUSED(unused);
}

block::~block() {
    if (!is_valid) {
        DEBUG_PRINT("[B][destructor]: not a valid block, skipping\n");
        return;
    }
    if (is_modified && !is_removed) {
        storage_block b(context.compressor->get_bufsize(dec_size));
        b.original_size = dec_size;

        int ret = context.compressor->compress(b.data.get(), &b.size, dec_data.get(), dec_size);
        if (ret != 0 || b.size > dec_size) {
            if (ret != 0) {
                WARNING_PRINT("warning: compressor->compress returned %d\n", ret);
            }

            b.is_compressed = false;
            b.data = std::move(dec_data);
            b.size = dec_size;
        } else {
            b.is_compressed = true;
        }

        if (addr != 0) {
            // block was read from file, so addr was already allocated from allocator previously

            // if (c_size >= b.size) {
            //     // we can reuse previous memory block in file
            // }

            // though it would be better to pass this logic to allocator, and allocate memory again
            context.allocator->deallocate(addr, c_size);
        }

        uint64_t new_addr = context.allocator->allocate(STORAGE_BLOCK_METASIZE + b.size);
        DEBUG_PRINT(
            "[B][destructor]: writing to file "
            "(new_addr=%lu,addr=%lu,original_size=%lu,size=%lu,is_compressed=%d,key.pos=%lu)\n",
            new_addr, addr, b.original_size, b.size, b.is_compressed, key.pos);
        b.write_to(context.file, new_addr);

        // block already in btree thanks to storage_block_reader
        // we just need to update it's file address
        context.index->update(key, {new_addr, dec_size});

        if (context.is_temporary_index_enabled) {
            // save allocated address to temporary_index
            context.temporary_index[key] = new_addr;
        }
    } else {
        if (addr == 0) {
            // this block was created in storage_block_reader::create_block, so it's key was
            // inserted to btree
            //
            // not calling index->remove, because storage_block_reader::remove_block calls it
            // index->remove(key);
        }
        if (is_removed && addr != 0) {
            // this block was created in storage_block_reader::read_block, so we need to deallocate
            // it's memory
            context.allocator->deallocate(addr, c_size);
        }
    }
}

const uint8_t *block::data() const { return dec_data.get(); }

uint8_t *block::data() {
    is_modified = true;
    return dec_data.get();
}

bool block::valid() const { return is_valid; }

uint64_t block::size() const { return dec_size; }

void block::shrink(uint64_t new_size) {
    assert(new_size < dec_size);
    is_modified = true;
    dec_size = new_size;
    // we're not updating size in btree, because while this block is in cache, read_block()->size()
    // will return correct size, and if block is not in cache, then destructor already updated size
    //
    // UPD: we are updating size in btree, because otherwise we can't use btree::get_block in
    // compio_insert
    context.index->update(key, {addr, new_size});
}

void block::remove() { is_removed = true; }

const tree_key &block::get_key() const { return key; }

void block::shift_key(int64_t addition) {
    assert(addition != 0);
    is_modified = true;
    DEBUG_PRINT("[B][shift_key]: key.pos=%lu, shifting with addition=%ld\n", key.pos, addition);
    key += addition;
}

void block::set_key(const tree_key &new_key) {
    if (key != new_key) {
        is_modified = true;
        key = new_key;
    }
}

storage_block_reader::storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                                           const compio_compressor *compressor, int max_size)
    : cache(max_size),
      context({file, allocator, index, compressor, {}, false}) {}

std::shared_ptr<block> storage_block_reader::read_block(uint64_t addr, tree_key key) {
    DEBUG_PRINT("[SBR][read_block]: addr=%lu, key.hash=%lu, key.pos=%lu\n", addr, key.hash,
                key.pos);
    if (cache.exists(key)) {
        DEBUG_PRINT("[SBR][read_block]: cache hit\n");
        return cache.get(key);
    }
    DEBUG_PRINT("[SBR][read_block]: cache miss\n");
    if (context.is_temporary_index_enabled) {
        auto it = context.temporary_index.find(key);
        if (it != context.temporary_index.end()) {
            addr = it->second;
            DEBUG_PRINT("[SBR][read_block]: getting addr from temporary_index: addr=%lu\n", addr);
        }
#ifndef NDEBUG
        auto val = context.index->get(key);
        assert(val.has_value());
        assert(val.value().addr == addr);
#endif
    }
    auto b = std::make_shared<block>(context, key, addr);
    if (!b->valid()) {
        return nullptr;
    }
    cache.put(key, b);
    return b;
}

std::shared_ptr<block> storage_block_reader::create_block(uint64_t size, tree_key key) {
    DEBUG_PRINT("[SBR][create_block]: size=%lu, key.hash=%lu, key.pos=%lu\n", size, key.hash,
                key.pos);
    if (cache.exists(key)) {
        WARNING_PRINT("warning: trying to create block with key (%lu, %lu), that "
                      "already exists in storage_block_reader.cache\n",
                      key.hash, key.pos);
        return cache.get(key);
    }
    auto b = std::make_shared<block>(context, key, size, false);
    cache.put(key, b);

    // adding element to btree, but without file address (we didn't allocate memory block yet)
    // block::~block will update this element in btree with new address
    context.index->insert(key, {0, size});
    return b;
}

void storage_block_reader::clear_cache() {
    DEBUG_PRINT("[SBR][clear_cache]\n");
    cache.clear();
}

void storage_block_reader::enable_temporary_index() { context.is_temporary_index_enabled = true; }

void storage_block_reader::disable_temporary_index() {
    context.is_temporary_index_enabled = false;
    context.temporary_index.clear();
}

void storage_block_reader::add_to_range(int64_t addition, const tree_key &key_min,
                                        const tree_key &key_max) {
    DEBUG_PRINT("[SBR][add_to_range]: adding %ld to range [%lu, %lu]\n", addition, key_min.pos,
                key_max.pos);
    // manually update block::key for entries in cache
    auto it_start = cache._cache_items_map.lower_bound(key_min);
    auto it_end = cache._cache_items_map.upper_bound(key_max);
    for (auto it = it_start; it != it_end; ++it) {
        it->second->second->shift_key(addition);
    }

    // and then update keys themselves
    cache.add_to_range(addition, key_min, key_max);
}

void storage_block_reader::remove_block(std::shared_ptr<block> block) {
    DEBUG_PRINT("[SBR]removing block with key.pos=%lu\n", block->get_key().pos);
    block->remove();
    const auto &key = block->get_key();
    if (cache.exists(key)) {
        cache.remove(key);
    }
    if (context.is_temporary_index_enabled) {
        context.temporary_index.erase(key);
    }
    context.index->remove(key);
}

bool storage_block_reader::cache_contains(const tree_key &key) const { return cache.exists(key); }

} // namespace compio
