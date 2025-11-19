#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include <memory>

#include "compio/infile_object.hpp"
#include "compio/storage_block_reader.hpp"

// forward declaration
namespace compio {

struct btree;

} // namespace compio

struct compio_archive {
    FILE *file;
    const compio_config config;
    smart_infile_object<compio::header> header;
    compio::btree *index;
    compio::storage_block_reader *block_reader;
    compio::block_allocator *allocator;
    uint8_t mode_b;

    compio_archive(FILE *file, uint8_t mode_b, const compio_config *config);
    bool is_readonly() const;
};

struct compio_file {
    compio_archive *archive;
    char name[COMPIO_FNAME_MAX_SIZE];
    uint64_t cursor;
    uint64_t size;
    uint64_t hash;
};

#endif // COMPIO_FILE_HEADER_
