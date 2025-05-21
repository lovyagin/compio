#include "storage_block_reader.hpp"

#include "debug_print.hpp"

namespace compio {

storage_block_reader::storage_block_reader(FILE* file, int max_size) : file(file), cache(max_size) {}

smart_infile_object<storage_block> storage_block_reader::read_block(uint64_t addr) {
    if (!cache.exists(addr)) {
        auto result = smart_infile_object<storage_block>(file, addr);
        cache.put(addr, result);
        return result;
    } else {
        return cache.get(addr);
    }
}

smart_infile_object<storage_block> storage_block_reader::create_block(uint64_t addr, std::unique_ptr<uint8_t[]>&& data, uint64_t size) {
    auto result = smart_infile_object<storage_block>(file, addr, new storage_block(std::move(data), size));
    cache.put(addr, result);
    return result;
}

void storage_block_reader::remove_block(smart_infile_object<storage_block> block) {
    if (!cache.exists(block.addr())) {
        WARNING_PRINT("warning: trying to remove non-existing storage block\n");
        return;
    }
    cache.remove(block.addr());
}

void storage_block_reader::clear_cache() {
    cache.clear();
}


} // namespace compio
