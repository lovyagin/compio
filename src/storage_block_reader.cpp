#include "compio/storage_block_reader.hpp"

#include "compio/debug_print.hpp"

namespace compio {

block::block(FILE *file, block_allocator *allocator, const compio_compressor *compressor,
             tree_key key, uint64_t addr)
    : file(file),
      allocator(allocator),
      compressor(compressor),
      key(key),
      addr(addr),
      c_size(0),
      dec_data(nullptr),
      dec_size(0),
      is_modified(false),
      is_removed(false),
      is_valid(true) {

    storage_block b;
    b.read_from(file, addr);

    c_size = b.size;
    dec_size = b.original_size;

    if (b.is_compressed) {
        dec_data = std::make_unique<uint8_t[]>(dec_size);
        int ret = compressor->decompress(dec_data.get(), &dec_size, b.data.get(), b.size);
        if (ret != 0) {
            WARNING_PRINT("compressed data is too big after decompression (%lu is not enough)\n",
                          dec_size);
            is_valid = false;
            return;
        }
    } else {
        dec_data = std::move(b.data);
    }
}

block::block(FILE *file, block_allocator *allocator, const compio_compressor *compressor,
             tree_key key, uint64_t size, std::unique_ptr<uint8_t[]> &&data)
    : file(file),
      allocator(allocator),
      compressor(compressor),
      key(key),
      addr(0),
      c_size(0),
      dec_data(std::move(data)),
      dec_size(size),
      is_modified(true),
      is_removed(false),
      is_valid(true) {}

// block::block(FILE *file, block_allocator *allocator, compio_compressor *compressor, tree_key key,
//              uint64_t size)
//     : block(file, allocator, compressor, key, size, std::make_unique<uint8_t[]>(size)) {}

block::~block() {
    if (is_valid && is_modified && !is_removed) {
        uint64_t c_buffer_size = compressor->get_bufsize(dec_size);
        storage_block b(c_buffer_size);
        b.original_size = dec_size;
        b.index_key = key;

        int ret = compressor->compress(b.data.get(), &c_buffer_size, dec_data.get(), dec_size);
        if (ret != 0 || c_buffer_size > dec_size) {
            if (ret != 0) {
                WARNING_PRINT("compressor->compress returned %d\n", ret);
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
            allocator->deallocate(addr, c_size);
        }

        uint64_t new_addr = allocator->allocate(STORAGE_BLOCK_METASIZE + b.size);
        b.write_to(file, new_addr);
    } else if (is_valid && is_removed && addr != 0) {
        allocator->deallocate(addr, c_size);
    }
}

const uint8_t *block::data() const { return dec_data.get(); }

uint8_t *block::data() { return dec_data.get(); }

storage_block_reader::storage_block_reader(FILE *file, block_allocator *allocator,
                                           const compio_compressor *compressor, int max_size)
    : file(file),
      cache(max_size),
      allocator(allocator),
      compressor(compressor) {}

smart_infile_object<storage_block> storage_block_reader::read_block(uint64_t addr) {
    if (!cache.exists(addr)) {
        auto result = smart_infile_object<storage_block>(file, addr);
        cache.put(addr, result);
        return result;
    } else {
        return cache.get(addr);
    }
}

smart_infile_object<storage_block>
storage_block_reader::create_block(uint64_t addr, std::unique_ptr<uint8_t[]> &&data,
                                   uint64_t size) {
    auto result =
        smart_infile_object<storage_block>(file, addr, new storage_block(std::move(data), size));
    cache.put(addr, result);
    return result;
}

void storage_block_reader::remove_block(uint64_t addr) {
    if (!cache.exists(addr)) {
        return;
    }
    auto block = cache.get(addr);
    block.remove();
    cache.remove(addr);
}

void storage_block_reader::clear_cache() { cache.clear(); }

} // namespace compio
