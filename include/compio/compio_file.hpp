#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include <memory>
#include <shared_mutex>
#include <mutex>

#include "compio/infile_object.hpp"
#include "compio/storage_block_reader.hpp"
#include "compio/wal.hpp"

// forward declaration
namespace compio {

struct btree;

} // namespace compio

struct compio_archive {
    std::shared_mutex mutex;
    std::mutex io_mutex;
    std::mutex header_mutex;
    std::mutex allocator_mutex;

    int current_header_slot; // 0 for A (offset 0), 1 for B (offset disk_size)
    FILE *file;
    const compio_config config;
    smart_infile_object<compio::header> header;
    compio::btree *index;
    compio::storage_block_reader *block_reader;
    compio::block_allocator *allocator;
    std::unique_ptr<compio::WalManager> wal;
    std::string path;
    uint8_t mode_b;
    uint32_t open_files_count;

    compio_archive(std::unique_ptr<compio::WalManager> wal, FILE *file, uint8_t mode_b, const compio_config *config);
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
