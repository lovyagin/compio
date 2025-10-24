#include "compio/storage_block_reader.hpp"

#include "compio/debug_print.hpp"

namespace compio {

block::block(FILE *file, block_allocator *allocator, btree *index,
             const compio_compressor *compressor, uint64_t addr)
    : file(file),
      allocator(allocator),
      index(index),
      compressor(compressor),
      key({0, 0}),
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
    key = b.index_key;

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

block::block(FILE *file, block_allocator *allocator, btree *index,
             const compio_compressor *compressor, tree_key key, uint64_t size,
             std::unique_ptr<uint8_t[]> &&data)
    : file(file),
      allocator(allocator),
      index(index),
      compressor(compressor),
      key(key),
      addr(0),
      c_size(0),
      dec_data(std::move(data)),
      dec_size(size),
      is_modified(true),
      is_removed(false),
      is_valid(true) {}

block::block(FILE *file, block_allocator *allocator, btree *index,
             const compio_compressor *compressor, tree_key key, uint64_t size,
             bool initialize_with_zeros)
    : block(file, allocator, index, compressor, key, size, std::make_unique<uint8_t[]>(size)) {
    if (initialize_with_zeros) {
        std::fill_n(dec_data.get(), size, 0);
    }
}

block::~block() {
    if (is_valid && is_modified && !is_removed) {
        storage_block b(compressor->get_bufsize(dec_size));
        b.original_size = dec_size;
        b.index_key = key;

        int ret = compressor->compress(b.data.get(), &b.size, dec_data.get(), dec_size);
        if (ret != 0 || b.size > dec_size) {
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

        if (addr == 0) {
            // this block was created with block_reader::create_block
            index->insert(key, {new_addr, dec_size});
        } else {
            // this block was read from file with block_reader::read_block
            index->update(key, {new_addr, dec_size});
        }
    } else if (is_valid && is_removed && addr != 0) {
        allocator->deallocate(addr, c_size);
    }
}

const uint8_t *block::data() const { return dec_data.get(); }

uint8_t *block::data() {
    is_modified = true;
    return dec_data.get();
}

storage_block_reader::storage_block_reader(FILE *file, block_allocator *allocator, btree *index,
                                           const compio_compressor *compressor, int max_size)
    : file(file),
      allocator(allocator),
      index(index),
      compressor(compressor) {
    UNUSED(max_size);
}

std::shared_ptr<block> storage_block_reader::read_block(uint64_t addr) {
    return std::make_shared<block>(file, allocator, index, compressor, addr);
}

std::shared_ptr<block> storage_block_reader::create_block(uint64_t size, tree_key key) {
    return std::make_shared<block>(file, allocator, index, compressor, key, size, true);
}

} // namespace compio
