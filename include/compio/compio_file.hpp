#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "compio/compressor_set.hpp"
#include "compio/infile_object.hpp"
#include "compio/storage_block_reader.hpp"
#include "compio/wal.hpp"
#include "compio/file.hpp"
#include "compio/tree_types.hpp"

// forward declaration
namespace compio {

struct btree;

} // namespace compio

struct compio_file;

struct compio_archive {
    std::shared_mutex mutex;
    std::mutex io_mutex;
    std::mutex header_mutex;
    std::shared_mutex allocator_mutex;

    int current_header_slot; // 0 for A (offset 0), 1 for B (offset disk_size)
    // The mutable header may already point to an unpublished allocator state.
    uint64_t published_allocator_state_offset = 0;
    uint64_t published_allocator_state_size = 0;
    uint64_t published_file_size = 0;
    FILE *file;
    const compio_config config;
    smart_infile_object<compio::header> header;
    compio::btree *index;
    compio::compressor_set compressors; // outlives the block reader, whose blocks use it
    compio::storage_block_reader *block_reader;
    compio::block_allocator *allocator;
    std::unique_ptr<compio::WalManager> wal;
    std::string path;
    uint8_t mode_b;
    // A compaction drops the index caches of these files: they hold block and node addresses.
    std::unordered_set<compio_file *> open_files;
    std::vector<compio_file *> auto_batching_files; /**< Files that hold a batch opened automatically */
    // Makes the in-memory header durable (data is synced first).
    std::function<bool()> publish_header;
    // Flushes everything and publishes a header. Lets the allocator checkpoint
    // between the rounds of a compaction.
    std::function<bool()> checkpoint;
    // Set by a compaction: the physical file is cut down to header->file_size
    // once the next header is durable.
    bool truncate_after_publish = false;
    // True while a compaction runs: the allocator state then moves down into free space.
    bool compacting = false;
    // CRC32C of the serialized files table the header references, so that a
    // flush packs the table only when it changed.
    uint32_t stored_table_crc = 0;

    compio_archive(std::unique_ptr<compio::WalManager> wal, FILE *file, uint8_t mode_b, const compio_config *config);
    bool is_readonly() const;
};

struct compio_file {
    compio_archive *archive;
    std::string name;
    uint64_t cursor;
    uint64_t size;
    uint64_t hash;
    // Cached index node from the B-tree; may be a leaf or an internal node.
    smart_infile_object<compio::index_node> cached_leaf;
    
    // Auto-batching state for sequential operations
    int auto_batch_count;           // Current count of operations in auto-batch
    uint64_t last_operation_end;    // End offset of last operation (offset + size)
    bool is_auto_batching;         // Whether we're currently in an auto-batch
    
    // Range caching for B-tree operations
    std::vector<std::pair<compio::tree_key, compio::tree_val>> cached_range;  // Cached range results
    uint64_t cached_range_min;                                                // Minimum offset covered by cache
    uint64_t cached_range_max;                                                // Maximum offset covered by cache
};

#endif // COMPIO_FILE_HEADER_
