#include "allocator.hpp"
#include <cstdlib>

namespace compio {

uint64_t allocate_block(compio_archive* archive, uint64_t size) {
    // TODO: create table of free blocks and use it instead
    auto block_start = archive->header->file_size;
    archive->header->file_size += size;
    printf("Allocated block of size %ld on %ld\n", size, block_start);
    return block_start;
}

void free_block(compio_archive* archive, uint64_t addr, uint64_t size) {
    // TODO: save that block into table of free blocks
    printf("Freed block of size %ld on %ld\n", size, addr);
    if (archive->config->fill_holes_with_zeros) {
        fseek(archive->file, addr, SEEK_SET);
        uint8_t* zeros = (uint8_t*)calloc(size, 1);
        fwrite(zeros, 1, size, archive->file);
    }
}

} // namespace compio