#ifndef STORAGE_BLOCK_READER_HPP_
#define STORAGE_BLOCK_READER_HPP_

#include "file.hpp"
#include "infile_object.hpp"
#include "third_party/lrucache.hpp"

namespace compio {

struct storage_block_reader {
    storage_block_reader(FILE* file, int max_size);
    smart_infile_object<storage_block> read_block(uint64_t addr);
    smart_infile_object<storage_block> create_block(uint64_t addr, std::vector<uint8_t>&& data);
    void remove_block(smart_infile_object<storage_block> block);
    void clear_cache();
    
private:
    FILE* file;
    cache::lru_cache<uint64_t, smart_infile_object<storage_block>> cache;
};

} // namespace compio


#endif // STORAGE_BLOCK_READER_HPP_