#include "compio.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <memory>
#include <utility>
#include <cerrno>
#include <filesystem>
#include <map>
#include <vector>
#include <set>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"
#include "compio/file.hpp"
#include "compio/utils.hpp"
#include "compio/btree.hpp"

using namespace compio;
namespace fs = std::filesystem;

void compio_build_default_config(compio_config *result) {
    result->b_tree_degree = 42;
    compio_build_lz4_compressor(&result->compressor);
#ifdef NDEBUG
    result->fill_holes_with_zeros = false;
#else
    result->fill_holes_with_zeros = true;
#endif
    result->block_size = 1 << 14;        // 16KB - balanced for performance and fragmentation
    result->block_size__minimum = 1 << 9;   // 512B min (compatible with test overrides)
    result->block_size__maximum = 1 << 18;  // 256KB max
    result->cache_size__nodes = 1024;
    result->cache_size__blocks = 8192;
    result->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    result->fragmentation_threshold = 30;
    result->max_files = COMPIO_MAX_FILES;
    result->wal_max_size_bytes = 64 * 1024 * 1024; // 64 MB
    result->checksum_type = COMPIO_CHECKSUM_CRC32C;
    result->wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    result->auto_batch_size = 8; // Conservative default for performance
}

int compio_get_compression_type(const char *fp, compio_compression_type *t) {
    FILE *file = fopen(fp, "r");
    if (file == nullptr) {
        return -1;
    }

    if (is_file_empty(file)) {
        fclose(file);
        return -2;
    }

    header h;
    if (!h.load_and_validate(file, 0)) {
        // If slot 0 is invalid, try slot 1 if possible.
        // We can't reliably know offset of slot 1 without valid slot 0 (because disk_size depends on max_files).
        // However, we can try to read max_files from slot 0 even if checksum fails?
        // Or assume default max_files?
        // For now, fail gracefully instead of asserting.
        WARNING_PRINT("warning: compio_get_compression_type: header at slot 0 is invalid/corrupted.\n");
        fclose(file);
        return -3; // Corruption error
    }
    *t = (compio_compression_type)h.compression_type;

    fclose(file);
    return 0;
}

static smart_infile_object<compio::header> load_header_double_buffered(FILE *file, std::mutex *io_mutex, int &out_slot) {
    if (is_file_empty(file)) {
         out_slot = 0;
         return smart_infile_object<header>(file, 0, new compio::header(), io_mutex);
    }

    header hA;
    bool validA = hA.load_and_validate(file, 0);
    
    // Calculate where B should be based on A (if A is valid)
    uint64_t offsetB = hA.disk_size(); 
    header hB;
    bool validB = hB.load_and_validate(file, offsetB);
    
    int chosen_slot = 0;
    header* chosen_h = nullptr;
    
    if (validA && validB) {
        if (hA.sequence_id >= hB.sequence_id) {
            chosen_slot = 0;
            chosen_h = new compio::header(std::move(hA));
        } else {
            chosen_slot = 1;
            chosen_h = new compio::header(std::move(hB));
        }
    } else if (validA) {
        chosen_slot = 0;
        chosen_h = new compio::header(std::move(hA));
    } else if (validB) {
        chosen_slot = 1;
        chosen_h = new compio::header(std::move(hB));
    } else {
        WARNING_PRINT("CRITICAL: Both archive headers are corrupted. Unable to open archive.\n");
        throw std::runtime_error("Both archive headers are corrupted");
    }
    
    out_slot = chosen_slot;
    uint64_t chosen_addr = (chosen_slot == 0) ? 0 : offsetB;
    
    // Return smart object marked as 'loaded_from_disk' (clean)
    return smart_infile_object<compio::header>(file, chosen_addr, chosen_h, io_mutex, true);
}

compio_archive::compio_archive(std::unique_ptr<compio::WalManager> wal, FILE *file, uint8_t mode_b, const compio_config *config)
    : current_header_slot(is_file_empty(file) ? 1 : 0),
      file(file),
      config(*config),
      header(is_file_empty(file)
                 ? smart_infile_object<compio::header>(file, 0,
                                                       new compio::header(static_cast<uint32_t>(config->max_files)),
                                                       &io_mutex)
                 : load_header_double_buffered(file, &io_mutex, current_header_slot)),
      index(nullptr),
      block_reader(nullptr),
      allocator(nullptr),
      wal(std::move(wal)),
      mode_b(mode_b),
      open_files_count(0) {
    // btree constructor is called in compio_open_archive to break
    // the dependence cycle (archive -> index -> allocator -> archive)
    //
    // so if you use compio_archive constructor directly (without compio_open_archive),
    // you should call archive->index = new btree(archive) after this constructor call
    //
    // index = new btree(this);
}

bool compio_archive::is_readonly() const {
    if (mode_b & mode_bit::plus) return false;
    return mode_b & mode_bit::r;
}

static bool validate_config(const compio_config *c, bool allow_zeros = false) {
    if (c->block_size <= 0 && !allow_zeros) {
        WARNING_PRINT("warning: block_size=%d <= 0\n", c->block_size);
        return false;
    }
    if (c->block_size__minimum < 0) {
        WARNING_PRINT("warning: block_size__minimum=%d < 0\n", c->block_size__minimum);
        return false;
    }
    if (!allow_zeros) {
        if (c->block_size__minimum > c->block_size) {
            WARNING_PRINT("warning: block_size__minimum=%d > block_size=%d\n", c->block_size__minimum,
                        c->block_size);
            return false;
        }
        if (c->block_size__maximum < c->block_size * 2) {
            WARNING_PRINT("warning: block_size__maximum=%d < block_size*2=%d\n", c->block_size__maximum,
                        c->block_size * 2);
            return false;
        }
    }
    if (c->b_tree_degree <= 0 && !allow_zeros) {
        WARNING_PRINT("warning: b_tree_degree=%d <= 0\n", c->b_tree_degree);
        return false;
    }
    if (c->cache_size__blocks < 0) {
        WARNING_PRINT("warning: cache_size__blocks=%d < 0\n", c->cache_size__blocks);
        return false;
    }
    if (c->cache_size__nodes < 4) {
        // with too small cache_size__nodes a bug appears, when nodes get evicted from cache, 
        // but still exist in local variables of some function, and if that function modifies 
        // that node, but some other function will try to read that node from file, it would get it's old version
        WARNING_PRINT("warning: cache_size__nodes=%d < 4\nplease use cache_size__nodes >= 4 ", c->cache_size__nodes);
        return false;
    }
    if (c->max_files <= 0 && !allow_zeros) {
        WARNING_PRINT("warning: max_files=%d <= 0\n", c->max_files);
        return false;
    }
    if (c->max_files > COMPIO_MAX_FILES_LIMIT) {
        WARNING_PRINT("warning: max_files=%d exceeds hard limit %d\n", c->max_files,
                      COMPIO_MAX_FILES_LIMIT);
        return false;
    }
    return true;
}

compio_archive *compio_open_archive(const char *fp, const char *mode, const compio_config *c) {
    if (!validate_config(c, true)) {
        errno = EINVAL;
        return NULL;
    }

    uint8_t mode_b;
    mode_b = parse_mode(mode);
    if (!mode_b) {
        errno = EINVAL;
        return NULL;
    }

    // The library manages file positions itself via fseek+fread/fwrite, so the
    // underlying FILE* must NOT have POSIX O_APPEND semantics (which would force
    // every write to EOF regardless of fseek). That rules out fopen("a"/"a+").
    //
    // Mapping to fopen modes:
    //   'r'      -> "rb"   (read-only)
    //   'r+'     -> "rb+"  (read/write, file must exist)
    //   'w'/'w+' -> "wb+"  (truncate or create, read/write)
    //   'a'/'a+' -> "rb+"  (read/write, no truncate); create-if-missing
    //              fallback to "wb+". Append semantics are enforced at the
    //              logical level by compio_open_file (cursor = file->size).
    const char *archive_open_mode;
    if (mode_b & mode_bit::w) {
        archive_open_mode = "wb+";
    } else if (mode_b & mode_bit::a) {
        archive_open_mode = "rb+";
    } else if (mode_b & mode_bit::plus) {
        archive_open_mode = "rb+";
    } else {
        archive_open_mode = "rb";
    }

    FILE *file = fopen(fp, archive_open_mode);
    if (file == nullptr && (mode_b & mode_bit::a)) {
        // 'a'/'a+' must create the file if it does not exist.
        file = fopen(fp, "wb+");
    }
    if (file == nullptr) {
        return NULL;
    }

    // Optimize stdio buffering for sequential access (64KB buffer)
    // This reduces syscalls significantly for sequential reads/writes
    if (setvbuf(file, nullptr, _IOFBF, 64 * 1024) != 0) {
        WARNING_PRINT("warning: failed to set stdio buffer size\n");
    }

    // Initialize WAL Manager
    auto wal = std::make_unique<compio::WalManager>(fp);
    if (c) {
        wal->set_sync_mode(c->wal_sync_mode);
    }
    
    // Check for recovery (only if we are not creating a new file from scratch with "w")
    if (!(mode_b & mode_bit::w)) {
        if (wal->has_pending_recovery()) {
            // Recovery needs read-write access to the archive file. 'r' alone is
            // read-only; everything else (r+, a, a+, w, w+) can write.
            bool can_write = !((mode_b & mode_bit::r) && !(mode_b & mode_bit::plus));

            if (!can_write) {
                 WARNING_PRINT("error: WAL file exists but opening in read-only mode. Cannot recover pending transactions.\n");
                 fclose(file);
                 errno = EROFS; // Read-only file system (or similar)
                 return NULL;
            } else {
                if (!wal->recover(file)) {
                    WARNING_PRINT("warning: WAL recovery failed\n");
                    // Continue anyway? Or fail? 
                    // Fail seems safer.
                    fclose(file);
                    errno = EIO;
                    return NULL;
                }
            }
        }
    } else {
        // "w" mode: truncate file. We should also clear any existing WAL.
        if (!wal->clear()) {
            WARNING_PRINT("warning: failed to clear existing WAL file in 'w' mode\n");
            fclose(file);
            errno = EIO;
            return NULL;
        }
    }
    
    // Open WAL for writing if we are in write mode
    // We can write if w, a, or + is set.
    bool can_write_archive = (mode_b & mode_bit::w) || (mode_b & mode_bit::a) || (mode_b & mode_bit::plus);
    if (can_write_archive) {
        if (!wal->open()) {
             WARNING_PRINT("warning: failed to open WAL file\n");
             // Fail?
             fclose(file);
             errno = EIO;
             return NULL;
        }
    }

    compio_config local_config = *c;
    bool is_new_file = is_file_empty(file);
    if (is_new_file) {
        if (local_config.max_files == 0) {
            local_config.max_files = COMPIO_MAX_FILES;
        }
    }

    compio_archive *archive = nullptr;
    try {
        archive = new compio_archive(std::move(wal), file, mode_b, &local_config);
    } catch (const std::exception& e) {
        WARNING_PRINT("warning: failed to initialize compio_archive: %s\n", e.what());
        fclose(file);
        errno = EINVAL;
        return NULL;
    }

    if (!archive) {
        WARNING_PRINT("warning: failed to allocate memory for compio_archive\n");
        goto no_archive;
    }

    // is_new_file already computed above

    if (is_new_file) {
        // For new files, we must have valid configuration (no zeros allowed)
        if (!validate_config(&local_config, false)) {
            errno = EINVAL;
            WARNING_PRINT("warning: cannot create new archive with zero parameters (auto-detect requires existing file)\n");
            goto no_allocator;
        }
    }

    if (!is_new_file) {
        // Validate that file is large enough to contain a complete header
        fseek64(file, 0, SEEK_END);
        int64_t actual_size = ftell64(file);
        if (actual_size < static_cast<int64_t>(sizeof(compio::header))) {
            WARNING_PRINT("warning: archive file truncated (size=%lld, need>=%" PRIu64 ")\n",
                          (long long)actual_size, (uint64_t)sizeof(compio::header));
            errno = EINVAL;
            goto no_allocator;
        }
    }
    if (is_new_file) {
        archive->header->compression_type = local_config.compressor.compression_type;
        archive->header->block_size = local_config.block_size;
        archive->header->b_tree_degree = local_config.b_tree_degree;
    } else {
        // "Smart Open" logic:
        // If config specifies 0 for a parameter, we use the value from the file.
        // Otherwise, we enforce the config value (validation).

        const auto& hdr = *readonly(archive->header, header);
        
        // 1. Compression Type
        if (hdr.compression_type != c->compressor.compression_type) {
             // For compression, we can't just "adopt" it because we need the function pointers
             // in c->compressor to match. If the user passed a compressor that doesn't match
             // the file's type, it's a hard error unless we implement a way to auto-switch compressors.
             // For now, we keep the existing strict check.
            errno = EINVAL;
            WARNING_PRINT("warning: compression type mismatch while opening archive\n");
            goto no_allocator;
        }

        // 2. Block Size
        if (c->block_size == 0) {
             // Auto-detect
             // We cast away constness because compio_archive stores a local copy of config,
             // and we need to update that copy to reflect the file's reality.
             const_cast<compio_config&>(archive->config).block_size = hdr.block_size;
        } else if (hdr.block_size != static_cast<uint32_t>(c->block_size)) {
            errno = EINVAL;
            WARNING_PRINT("warning: block_size mismatch while opening archive (file=%u, config=%d)\n",
                          hdr.block_size, c->block_size);
            goto no_allocator;
        }

        // 3. B-Tree Degree
        if (c->b_tree_degree == 0) {
             // Auto-detect: adopt the degree from the file and update both the archive
             // config and the local config pointer used later in this function.
             const_cast<compio_config&>(archive->config).b_tree_degree = hdr.b_tree_degree;
             const_cast<compio_config*>(c)->b_tree_degree = static_cast<int>(hdr.b_tree_degree);
        } else if (hdr.b_tree_degree != static_cast<uint32_t>(c->b_tree_degree)) {
            errno = EINVAL;
            WARNING_PRINT("warning: b_tree_degree mismatch while opening archive (file=%u, config=%d)\n",
                          hdr.b_tree_degree, c->b_tree_degree);
            goto no_allocator;
        }

        // 4. Max Files
        bool is_v5 = (hdr.magic_number == 27110662);
        if (c->max_files == 0) {
             const_cast<compio_config&>(archive->config).max_files = hdr.ftable.max_files;
        } else if (hdr.ftable.max_files != static_cast<uint32_t>(c->max_files)) {
            if (!is_v5) {
                errno = EINVAL;
                WARNING_PRINT("warning: max_files mismatch while opening archive (file=%u, config=%d)\n",
                              hdr.ftable.max_files, c->max_files);
                goto no_allocator;
            } else {
                // For v5, max_files in config is just a hint or minimum.
                // We adopt the actual capacity from the file.
                const_cast<compio_config&>(archive->config).max_files = hdr.ftable.max_files;
            }
        }
    }

    // initialize allocator before btree, because btree uses allocator for creating root node
    archive->allocator = new compio::block_allocator(archive, archive->wal.get());
    if (!archive->allocator) {
        WARNING_PRINT("warning: failed to allocate memory for allocator\n");
        goto no_allocator;
    }

    // is_readonly=true only for pure 'r' (no '+', 'w', or 'a' bits).
    // 'r+' sets both r and plus bits → it must be read-write so btree nodes are persisted.
    {
        bool btree_readonly = (mode_b & mode_bit::r) &&
                              !(mode_b & mode_bit::plus) &&
                              !(mode_b & mode_bit::w) &&
                              !(mode_b & mode_bit::a);
    archive->index = new btree(c->b_tree_degree, btree_readonly, archive->header,
                               archive->allocator, file, c->cache_size__nodes, &archive->io_mutex, archive->wal.get());
    }
    if (!archive->index) {
        WARNING_PRINT("warning: failed to allocate memory for btree\n");
        goto no_index;
    }

    archive->block_reader = new compio::storage_block_reader(
        file, archive->allocator, archive->index,
        &archive->config.compressor, archive->config.cache_size__blocks, &archive->io_mutex, archive->wal.get(),
        archive->config.checksum_type);
    if (!archive->block_reader) {
        WARNING_PRINT("warning: failed to allocate memory for storage_block_reader\n");
        goto no_block_reader;
    }

    if (!is_new_file && !archive->allocator->load_state(archive)) {
        WARNING_PRINT("warning: failed to load allocator state from archive\n");
        goto no_allocator_state;
    }

    return archive;

no_allocator_state:
    delete archive->block_reader;
no_block_reader:
    delete archive->index;
no_index:
    delete archive->allocator;
no_allocator:
    delete archive;
no_archive:
    fclose(file);
    return NULL;
}

compio_file *compio_open_file(const char *name, compio_archive *archive) {
    if (!archive) {
        return NULL;
    }
    if (!name) {
        errno = EINVAL;
        return NULL;
    }
    std::unique_lock<std::shared_mutex> lock(archive->mutex);
    size_t name_len = strlen(name);
    if (name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    auto file_table_item = readonly(archive->header, header)->ftable.find(name);
    if (file_table_item == nullptr) {
        if (!archive->is_readonly()) {
            bool allow_resize = (archive->header->magic_number == 27110662);
            file_table_item = archive->header->ftable.add(name, allow_resize);
            if (file_table_item == NULL) {
                errno = ENFILE;
                return NULL;
            }
        } else {
            errno = EROFS;
            return NULL;
        }
    }

    auto file = new compio_file();

    file->size = file_table_item->size;

    if (archive->mode_b & mode_bit::a)
        file->cursor = file->size;
    else
        file->cursor = 0;

    file->archive = archive;
    strncpy(file->name, name, COMPIO_FNAME_MAX_SIZE - 1);
    file->name[COMPIO_FNAME_MAX_SIZE - 1] = '\0';

    file->hash = fnv1a(name);

    // Initialize auto-batching state
    file->auto_batch_count = 0;
    file->last_operation_end = UINT64_MAX; // Invalid end to start
    file->is_auto_batching = false;

    // Initialize range cache state
    file->cached_range.clear();
    file->cached_range_min = 0;
    file->cached_range_max = 0;

    archive->open_files_count++;

    return file;
}

int compio_remove_file(compio_archive *archive, const char *name) {
    if (!archive) {
        return -1;
    }
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    std::unique_lock<std::shared_mutex> lock(archive->mutex);
    size_t name_len = strlen(name);
    if (name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return -2;
    }

    // Find file in file table
    auto file_table_item = readonly(archive->header, compio::header)->ftable.find(name);
    if (file_table_item == nullptr) {
        errno = ENOENT;
        return -1;
    }

    // Get all blocks belonging to this file
    const uint64_t file_size = file_table_item->size;
    const uint64_t hash = fnv1a(name);

    if (file_size > 0) {
        tree_key key_min = {hash, 0};
        tree_key key_max = {hash, UINT64_MAX}; // Get all blocks for this file
        auto all_blocks_opt = archive->index->get_range(key_min, key_max);
        if (!all_blocks_opt) {
            errno = EIO;
            return -1;
        }
        const auto& all_blocks = *all_blocks_opt;

    // Suspend maintenance to avoid re-entry during block removal
    // Use RAII guard to ensure maintenance is resumed even if an error occurs
    struct MaintenanceGuard {
        block_allocator* alloc;
        MaintenanceGuard(block_allocator* a) : alloc(a) { if(alloc) alloc->suspend_maintenance(); }
        ~MaintenanceGuard() { if(alloc) alloc->resume_maintenance(); }
    } maintenance_guard(archive->allocator);

    // Iterate over all blocks of the file
    {
        int64_t saved_pos = ftell64(archive->file);

        // Deallocate all blocks and remove them from index
        for (const auto &[key, val] : all_blocks) {
            if (archive->block_reader && archive->block_reader->cache_contains(key)) {
                 auto b = archive->block_reader->read_block(val.addr, key);
                 archive->block_reader->remove_block(b);
                 continue;
            }

            if (val.addr != 0) {
                // Read storage_block metadata to get compressed size
                storage_block sb;
                if (sb.read_from(archive->file, val.addr)) {
                    // Remove block from B-tree index BEFORE deallocating.
                    archive->index->remove(key);

                    // Deallocate. Maintenance is suspended globally, so this won't trigger defrag.
                    // Use overloaded deallocate with explicit false for perform_maintenance, although
                    // suspended state also prevents it.
                    archive->allocator->deallocate(val.addr, STORAGE_BLOCK_METASIZE + sb.size, false);
                } else {
                    WARNING_PRINT("warning: failed to read block at %" PRIu64 " during removal\n", val.addr);
                    if (saved_pos >= 0) {
                        fseek64(archive->file, saved_pos, SEEK_SET);
                    }
                    errno = EIO;
                    return -1;
                }
            } else {
                // Remove block from B-tree index (if addr is 0, it's still in index)
                archive->index->remove(key);
            }
        }

        // Restore file position
        if (saved_pos >= 0) {
            fseek64(archive->file, saved_pos, SEEK_SET);
        }
    }
    } // End if (file_size > 0)
    
    // Remove file from file table
    return archive->header->ftable.remove(name);
}

int compio_get_fragmentation_stats(compio_archive *archive, compio_fragmentation_stats *stats) {
    if (!archive || !stats) {
        return COMPIO_ERROR;
    }
    std::shared_lock<std::shared_mutex> lock(archive->mutex);

    if (!archive->allocator) {
        return COMPIO_ERROR;
    }

    // Get stats from allocator
    auto internal_stats = archive->allocator->get_fragmentation_stats();

    // Copy to C structure
    stats->num_free_regions = internal_stats.num_free_regions;
    stats->total_free_bytes = internal_stats.total_free_bytes;
    stats->largest_free_region = internal_stats.largest_free_region;
    stats->smallest_free_region = internal_stats.smallest_free_region;
    stats->avg_free_region_size = internal_stats.avg_free_region_size;
    stats->fragmentation_percent = internal_stats.fragmentation_percent;

    return COMPIO_SUCCESS;
}

// Auto-batching helper functions

// Forward declarations
static void end_auto_batch_if_active(compio_file *file);

/**
 * Check if current operation is sequential and should be auto-batched
 */
static bool should_auto_batch(compio_file *file, uint64_t current_offset) {
    if (!file || file->archive->config.auto_batch_size <= 0) {
        return false;
    }
    
    // If this is the first operation, it's sequential
    if (file->last_operation_end == UINT64_MAX) {
        return true;
    }
    
    // Treat operations as sequential as long as we do not seek past the last end.
    // This allows non-growing operations (e.g., erase) at the same offset to batch.
    return current_offset <= file->last_operation_end;
}

/**
 * Start auto-batching if conditions are met
 */
static void start_auto_batch_if_needed(compio_file *file, uint64_t current_offset, uint64_t size) {
    if (!should_auto_batch(file, current_offset)) {
        // Break existing auto-batch if pattern breaks
        end_auto_batch_if_active(file);
        file->auto_batch_count = 1; // Reset counter for new sequence
        file->last_operation_end = current_offset + size;
        return;
    }
    
    file->auto_batch_count++;
    file->last_operation_end = current_offset + size;
    
    // Start auto-batch when we reach the threshold
    if (!file->is_auto_batching && file->auto_batch_count >= 3) {
        if (compio_begin_batch(file->archive) == COMPIO_SUCCESS) {
            file->is_auto_batching = true;
            file->auto_batch_count = 1; // Reset counter to count batched operations
        }
        // On failure, silently continue without auto-batching
    }
}

/**
 * End auto-batch if active and threshold reached
 */
static void end_auto_batch_if_needed(compio_file *file) {
    if (file->is_auto_batching && 
        file->auto_batch_count >= file->archive->config.auto_batch_size) {
        compio_end_batch(file->archive); // Ignore return value - best effort
        file->is_auto_batching = false;
        file->auto_batch_count = 0;
    }
}

/**
 * Force end auto-batch if currently active
 */
static void end_auto_batch_if_active(compio_file *file) {
    if (file->is_auto_batching) {
        compio_end_batch(file->archive); // Best effort - ignore errors
        file->is_auto_batching = false;
        file->auto_batch_count = 0;
    }
}

/**
 * Check if a range is covered by the cached range
 */
static bool is_range_cached(compio_file *file, uint64_t offset_min, uint64_t offset_max) {
    return !file->cached_range.empty() && 
           offset_min >= file->cached_range_min && 
           offset_max <= file->cached_range_max;
}

/**
 * Get blocks from cache for the given range
 */
static std::vector<std::pair<compio::tree_key, compio::tree_val>> get_cached_range_blocks(
    compio_file *file, uint64_t offset_min, uint64_t offset_max) {
    
    std::vector<std::pair<compio::tree_key, compio::tree_val>> result;
    
    for (const auto& kv : file->cached_range) {
        const auto& key = kv.first;
        const auto& val = kv.second;
        
        // Skip blocks from other files
        if (key.hash != file->hash) continue;
        
        // Check if block overlaps with requested range
        uint64_t block_start = key.pos;
        uint64_t block_end = key.pos + val.size;
        
        if (block_start < offset_max && block_end > offset_min) {
            result.push_back(kv);
        }
    }
    
    return result;
}

/**
 * Cache range results and update cache bounds
 */
static void cache_range_results(compio_file *file, 
                               const std::vector<std::pair<compio::tree_key, compio::tree_val>>& range_results,
                               uint64_t /*offset_min*/, uint64_t /*offset_max*/) {
    
    file->cached_range = range_results;

    if (range_results.empty()) {
        file->cached_range_min = 0;
        file->cached_range_max = 0;
        return;
    }

    uint64_t min_pos = range_results.front().first.pos;
    uint64_t max_pos = range_results.front().first.pos + range_results.front().second.size;

    for (const auto &kv : range_results) {
        const auto &key = kv.first;
        const auto &val = kv.second;

        uint64_t block_start = key.pos;
        uint64_t block_end = key.pos + val.size;

        if (block_start < min_pos) {
            min_pos = block_start;
        }
        if (block_end > max_pos) {
            max_pos = block_end;
        }
    }

    file->cached_range_min = min_pos;
    file->cached_range_max = max_pos;
}

/**
 * Invalidate range cache (called on write/insert/erase operations)
 */
static void invalidate_range_cache(compio_file *file) {
    file->cached_range.clear();
    file->cached_range_min = 0;
    file->cached_range_max = 0;
}

int compio_begin_batch(compio_archive *archive) {
    if (archive == nullptr) {
        WARNING_PRINT("warning: passed nullptr into compio_begin_batch\n");
        errno = EINVAL;
        return COMPIO_ERROR;
    }

    if (archive->is_readonly()) {
        WARNING_PRINT("warning: compio_begin_batch called on read-only archive\n");
        errno = EACCES;
        return COMPIO_ERROR;
    }

    // No need for external lock - WalManager has its own mutex
    archive->wal->begin_batch();
    return COMPIO_SUCCESS;
}

int compio_end_batch(compio_archive *archive) {
    if (archive == nullptr) {
        WARNING_PRINT("warning: passed nullptr into compio_end_batch\n");
        errno = EINVAL;
        return COMPIO_ERROR;
    }

    if (archive->is_readonly()) {
        WARNING_PRINT("warning: compio_end_batch called on read-only archive\n");
        errno = EACCES;
        return COMPIO_ERROR;
    }

    // No need for external lock - WalManager has its own mutex
    bool success = archive->wal->end_batch(archive->file, archive->config.wal_max_size_bytes);
    
    if (!success) {
        if (errno == 0) {
            errno = EIO;
        }
        return COMPIO_ERROR;
    }
    
    return COMPIO_SUCCESS;
}

int compio_defragment(compio_archive *archive) {
    if (!archive) {
        WARNING_PRINT("warning: passed nullptr into compio_defragment\n");
        return COMPIO_ERROR;
    }
    std::unique_lock<std::shared_mutex> lock(archive->mutex);

    if (archive->mode_b & mode_bit::r) {
        WARNING_PRINT("warning: compio_defragment called on read-only archive\n");
        return COMPIO_ERROR;
    }

    if (!archive->allocator || !archive->index || !archive->file) {
        return COMPIO_ERROR;
    }

    if (archive->open_files_count > 0) {
        WARNING_PRINT("warning: compio_defragment called with %u open file(s)\n",
                       archive->open_files_count);
        return COMPIO_ERROR;
    }

    archive->allocator->force_defragmentation();
    return COMPIO_SUCCESS;
}

static void flush_header_double_buffered(compio_archive *archive) {
    if (!archive || !archive->header) return;

    // Double-buffered Header Write
    if (!(archive->mode_b & mode_bit::r)) {
        int target_slot = 1 - archive->current_header_slot;
        uint64_t target_addr = (target_slot == 0) ? 0 : archive->header->reserved_size() / 2;

        // Create new header data based on current in-memory header
        compio::header* new_h_data = new compio::header(*archive->header);
        new_h_data->sequence_id++;
        new_h_data->compute_checksum(new_h_data->checksum); // Ensure checksum is fresh

        // Write to the target (alternate) slot
        smart_infile_object<compio::header> new_header_obj(archive->file, target_addr, new_h_data, &archive->io_mutex, true);
        new_header_obj.write();

        // Switch internal state to point to the new slot
        archive->current_header_slot = target_slot;
        
        // Mark old header object as unmodified so it doesn't write on destruction
        archive->header.unmodify();
        
        // Move new header object into place
        archive->header = std::move(new_header_obj);

        // Update allocator's file_size pointer since header object moved
        if (archive->allocator) {
             archive->allocator->update_file_size_ptr(&archive->header->file_size);
        }
    }
}

int compio_close_file(compio_file *file) {
    if (!file || !file->archive) {
        WARNING_PRINT("warning: passed nullptr into compio_close_file\n");
        return -1;
    }
    std::unique_lock<std::shared_mutex> lock(file->archive->mutex);

    // End any active auto-batch before closing
    end_auto_batch_if_active(file);

    if (file->archive->open_files_count > 0) {
        file->archive->open_files_count--;
    }

    delete file;
    return 0;
}

int compio_close_archive(compio_archive *archive) {
    if (!archive) {
        WARNING_PRINT("warning: passed nullptr into compio_close_archive\n");
        return -1;
    }

    // 1) flush cached data to file (writes all dirty blocks; keeps block_reader valid)
    compio_flush(archive);

    // 2) run maintenance (defragmentation) before saving allocator state.
    //    Must happen while block_reader and index are still alive.
    if (!(archive->mode_b & mode_bit::r) && archive->allocator) {
        archive->allocator->maintenance();
    }

    // 3) now safe to delete block_reader
    delete archive->block_reader;
    archive->block_reader = nullptr;

    // 4) save allocator state to the end of the file, if not read-only mode
    if (!(archive->mode_b & mode_bit::r) && archive->allocator) {
        if (!archive->allocator->save_state(archive)) {
            WARNING_PRINT("warning: failed to save allocator state\n");
            return -3;
        }
    }

    // 5) delete allocator
    delete archive->allocator;
    archive->allocator = nullptr;

    // 6) delete btree (it actually depends on allocator, but allocator also depends on index,
    // however they don't call each other in their destructors, so their destruction order does not
    // matter)
    delete archive->index;
    archive->index = nullptr;

    // 7) flush header safely using double-buffering
    // This ensures that the final state (including allocator updates) is written atomically to the alternate slot.
    // If we crash during this write, the previous valid header (in the other slot) is preserved.
    flush_header_double_buffered(archive);

    // Ensure all data is physically on disk before clearing WAL.
    // This prevents data loss if power fails between WAL clear and fclose.
    if (fflush(archive->file) != 0) {
        WARNING_PRINT("warning: fflush failed in compio_close_archive\n");
    }
#ifdef _WIN32
    if (_commit(_fileno(archive->file)) != 0) {
        WARNING_PRINT("warning: _commit failed in compio_close_archive\n");
    }
#else
    if (fsync(fileno(archive->file)) != 0) {
        WARNING_PRINT("warning: fsync failed in compio_close_archive\n");
    }
#endif

    // If WAL is enabled and we are closing cleanly, we should clear the WAL.
    // At this point, all data is synced to the archive file (via flush and header update).
    // The WAL is redundant now.
    if (archive->wal) {
        // Commit any pending transaction before clearing (though close logic usually implies clean state)
        // Actually, we don't need commit here, as flush_header_double_buffered creates its own atomic write.
        // And compio_flush called earlier committed its changes.
        archive->wal->clear();
    }

    // Destroy the header object. It is now clean (unmodified) because flush_header_double_buffered just wrote it.
    archive->header = {};

    // 8) finally we close the file (block_reader, allocator and index are all deleted, so no
    // fwrites will be called after this)
    if (fclose(archive->file)) {
        WARNING_PRINT("warning: failed to close file in compio_close_archive\n");
        return -2;
    }
    DEBUG_PRINT("[cca]: closed file\n");

    // 9) and delete archive structure
    delete archive;

    return COMPIO_SUCCESS;
}

int compio_seek(compio_file *file, int64_t offset, uint8_t origin) {
    int64_t new_cursor = file->cursor;
    switch (origin) {
    case COMPIO_SEEK_SET:
        new_cursor = offset;
        break;
    case COMPIO_SEEK_CUR:
        new_cursor += offset;
        break;
    case COMPIO_SEEK_END:
        new_cursor = file->size + offset;
        break;
    }

    if (new_cursor < 0) {
        errno = EINVAL;
        return -1;
    }

    // End auto-batch if seek call happens (even no-op seeks)
    end_auto_batch_if_active(file);
    file->auto_batch_count = 0;
    file->last_operation_end = UINT64_MAX;

    file->cursor = new_cursor;
    DEBUG_PRINT("\ncompio_seek(new_cursor=%" PRId64 ")\n", new_cursor);
    return 0;
}

uint64_t compio_tell(compio_file *file) { return file->cursor; }

uint64_t compio_get_size(compio_file *file) { return file->size; }

static void validate_no_gaps_in_range(const std::vector<std::pair<tree_key, tree_val>> range) {
    for (std::size_t i = 1; i < range.size(); ++i) {
        // check that there's no gaps between blocks
        const auto &[key_prev, val_prev] = range[i - 1];
        const auto &[key, val] = range[i];
        (void)key_prev; (void)val_prev; (void)key; (void)val;
        assert(key.pos == key_prev.pos + val_prev.size);
    }
}

static void validate_tree(btree *index, compio_file *file, bool allow_empty = false) {
    (void)index; (void)file; (void)allow_empty;
#ifndef NDEBUG
    DEBUG_PRINT("[VALIDATE_TREE]: current btree state for file with hash=%" PRIu64 ":\n", file->hash);
    auto file_range_opt = index->get_range(tree_key{file->hash, 0}, tree_key{file->hash, UINT64_MAX});
    if (!file_range_opt) return; // ignore validation on failure
    const auto& file_range = *file_range_opt;

    if (!allow_empty) {
        assert(!file_range.empty());
    }
    validate_no_gaps_in_range(file_range);
    for (const auto &[key, val] : file_range) {
        assert(key.hash == file->hash);
    }
    if (!file_range.empty()) {
        // check that blocks cover the whole file
        assert(file_range.front().first.pos == 0);
#ifdef COMPIO_DISABLE_INSERT_ERASE
        assert(file_range.back().first.pos + file_range.back().second.size >= file->size);
#else
        assert(file_range.back().first.pos + file_range.back().second.size == file->size);
#endif
    }
#endif
}

// RAII guard for WAL transactions when not in batch mode
struct WalTransactionGuard {
    compio::WalManager* wal;
    bool active;

    explicit WalTransactionGuard(compio::WalManager* w)
        : wal(w), active(false) {
        if (wal && wal->get_batch_depth() == 0) {
            wal->begin_transaction();
            active = true;
        }
    }

    bool commit(FILE* archive_file = nullptr, uint64_t max_wal_size = 0) {
        if (!active || !wal) return true;
        bool result = wal->commit_transaction(archive_file, max_wal_size);
        active = false;
        return result;
    }

    void dismiss() noexcept {
        active = false;
    }

    ~WalTransactionGuard() {
        if (active && wal) {
            wal->rollback_transaction();
        }
    }
};

static uint64_t compio_write_impl(const void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_write_impl(cursor=%" PRIu64 ", size=%" PRIu64 ", file_size=%" PRIu64 ")\n", file->cursor, size, file->size);

    // Invalidate cached leaf, because write operation modifies the tree/blocks
    // Even if size is 0 or error occurs later, invalidating cache is safe (just a performance hit)
    // But we need it for correctness on successful writes.
    if (file) {
        file->cached_leaf = {};
        // Note: range cache invalidation moved to end after successful operations
    }

    const auto archive = file->archive;
    const auto block_reader = archive->block_reader;
    const uint64_t block_size = archive->config.block_size;
    const uint64_t block_size__minimum = archive->config.block_size__minimum;
    const uint64_t block_size__maximum = archive->config.block_size__maximum;
    (void)block_size__maximum;

    if ((archive->mode_b & mode_bit::r) && !(archive->mode_b & mode_bit::plus)) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

    if (size == 0) {
        return 0;
    }

    // Auto-batching logic - start batching if sequential
    start_auto_batch_if_needed(file, file->cursor, size);

    auto file_table_item = archive->header->ftable.find(file->name);
    if (!file_table_item) {
        // should not happen, because compio_open_file creates ftable record
        WARNING_PRINT("warning: no such file in header.ftable\n");
        errno = ENOENT;
        return 0;
    }

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    WalTransactionGuard wal_tx_guard(wal_ptr);

    // actual range in file, where we need to write (write-range)
    const uint64_t write_start = file->cursor;
    const uint64_t write_end = write_start + size;

    auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);
    uint64_t ptr_bytes_written = 0;

#ifdef COMPIO_DISABLE_INSERT_ERASE
    const uint64_t last_block_end = ((file->size + block_size - 1) / block_size) * block_size;
#else
    const uint64_t last_block_end = file->size;
#endif

    // if write-range intersects existing blocks, we need to modify them
    std::vector<std::pair<tree_key, tree_val>> range;
    if (write_start < last_block_end) {
        // Try to use cached range first
        if (is_range_cached(file, write_start, write_end)) {
            range = get_cached_range_blocks(file, write_start, write_end);
        } else {
            // Cache miss - get blocks range from b-tree
            const tree_key key_min = {file->hash, write_start};
            const tree_key key_max = {file->hash, write_end};
            auto range_opt = archive->index->get_range(key_min, key_max);
            if (!range_opt) {
                WARNING_PRINT("error: failed to read index range during write\n");
                errno = EIO;
                return 0;
            }
            range = *range_opt;
            
            // Cache the results for future operations
            cache_range_results(file, range, write_start, write_end);
        }
        // Gaps are allowed now, we will fill them
    }

    block_reader->enable_temporary_index();

    auto range_it = range.begin();

    while (ptr_bytes_written < size) {
        const uint64_t current_pos = write_start + ptr_bytes_written;
        
        // Find if we are inside an existing block
        bool inside_block = false;
        tree_key key{0, 0};
        tree_val val{0, 0};

        while (range_it != range.end()) {
            if (range_it->first.pos + range_it->second.size <= current_pos) {
                // Block is behind us, skip it
                range_it++;
                continue;
            }
            if (range_it->first.pos <= current_pos) {
                // We are inside this block
                inside_block = true;
                key = range_it->first;
                val = range_it->second;
            }
            break; // Found the relevant block (or the next one after gap)
        }

        if (inside_block) {
             // WRITE TO EXISTING BLOCK
            DEBUG_PRINT("[CW]reading block ({%" PRIu64 ",%" PRIu64 "}-{%" PRIu64 ",%" PRIu64 "})\n", key.hash, key.pos, val.addr,
                        val.size);
            const auto b = block_reader->read_block(val.addr, key);
            if (!b) {
                // failed to decompress
                WARNING_PRINT(
                    "warning: failed to decompress data (compressed block is corrupted)\n");
                errno = EIO;
                block_reader->disable_temporary_index();
                return ptr_bytes_written;
            }
            assert(b->size() == val.size);

            const uint64_t block_end = key.pos + b->size();
            const uint64_t copy_end = std::min(write_end, block_end);
            const uint64_t copy_size = copy_end - current_pos;
            const uint64_t dec_offset = current_pos - key.pos;
            
            DEBUG_PRINT("[CW] EXISTING BLOCK DATA: (%" PRIu64 ", %" PRIu64 ", %" PRIu64 ")\n", dec_offset, copy_size, b->size() - dec_offset - copy_size);

            std::copy_n(p_ptr, copy_size, b->data() + dec_offset);
            p_ptr += copy_size;
            ptr_bytes_written += copy_size;
            file->cursor += copy_size;
            file_table_item->size = file->size = std::max(file->cursor, file->size);
        } else {
            // WRITE TO NEW BLOCK (GAP or APPEND)
            // Determine size of new block
            uint64_t next_block_pos = (range_it != range.end()) ? range_it->first.pos : UINT64_MAX;
            // Limit gap size by write_end (we don't fill gap beyond what we write, unless we want to zero-fill? 
            
            uint64_t remaining_write = size - ptr_bytes_written;

#ifndef COMPIO_DISABLE_INSERT_ERASE
            // When appending at EOF, try to extend the current tail block instead of creating a
            // new one. This reduces internal fragmentation caused by many small appends.
            if (current_pos == file->size && current_pos > 0) {
                const tree_key tail_probe{file->hash, current_pos - 1};
                const auto tail_kv = archive->index->get_block(tail_probe);
                if (tail_kv.has_value()) {
                    const auto &[tail_key, tail_val] = tail_kv.value();
                    if (tail_key.hash == file->hash && tail_key.pos + tail_val.size == current_pos &&
                        tail_val.size < block_size__maximum) {
                        const auto tail_block = block_reader->read_block(tail_val.addr, tail_key);
                        if (!tail_block) {
                            WARNING_PRINT(
                                "warning: failed to decompress data (compressed block is corrupted)\n");
                            errno = EIO;
                            block_reader->disable_temporary_index();
                            return ptr_bytes_written;
                        }

                        const uint64_t append_size =
                            std::min<uint64_t>(remaining_write, block_size__maximum - tail_block->size());
                        if (append_size > 0) {
                            const uint64_t old_tail_size = tail_block->size();
                            tail_block->grow(old_tail_size + append_size);
                            std::copy_n(p_ptr, append_size, tail_block->data() + old_tail_size);

                            p_ptr += append_size;
                            ptr_bytes_written += append_size;
                            file->cursor += append_size;
                            file_table_item->size = file->size = std::max(file->size, current_pos + append_size);
                            continue;
                        }
                    }
                }
            }
#endif
            uint64_t current_block_size;
            
#ifdef COMPIO_DISABLE_INSERT_ERASE
            current_block_size = block_size;
#else
            uint64_t gap_constraint = (next_block_pos == UINT64_MAX) ? UINT64_MAX : (next_block_pos - current_pos);
            uint64_t available_space = std::min(remaining_write, gap_constraint);
            
            // If we are at EOF (next_block_pos == MAX), we use normal allocation logic.
            // If we are in a bounded gap, we fit into it.
            
            if (gap_constraint == UINT64_MAX) {
                 // Appending logic
                 if (remaining_write < block_size ||
                    remaining_write - block_size < block_size__minimum) {
                    current_block_size = remaining_write;
                 } else {
                    current_block_size = block_size;
                 }
            } else {
                // Filling gap logic
                if (available_space < block_size ||
                    (available_space >= block_size && available_space - block_size < block_size__minimum)) {
                    current_block_size = available_space;
                } else {
                    current_block_size = block_size;
                }
            }
#endif
            assert(current_block_size <= block_size__maximum);
            
            
            // Create new block
            const uint64_t copy_from_ptr = std::min(current_block_size, remaining_write);
             const uint64_t pad_size = current_block_size - copy_from_ptr;
             
             const tree_key key{file->hash, current_pos};
             DEBUG_PRINT("[CW]creating block ({%" PRIu64 ",%" PRIu64 "}-{?,%" PRIu64 "})\n", key.hash, key.pos,
                        current_block_size);
             const auto b = block_reader->create_block(current_block_size, key);

             std::copy_n(p_ptr, copy_from_ptr, b->data());
             if (pad_size > 0) {
                 std::fill_n(b->data() + copy_from_ptr, pad_size, 0);
             }
             
             p_ptr += copy_from_ptr;
             ptr_bytes_written += copy_from_ptr;
             file->cursor += copy_from_ptr; // cursor advances by bytes written from ptr? 
             // NO, cursor should advance by file space consumed!
             // If we write 1 byte and pad 15 bytes, cursor moves 1 or 16?
             // ftell usually returns logical position.
             // If we pad, we extended the file.
             // But if the user wrote 1 byte, they expect cursor to move by 1?
             // But we implicitly wrote zeros.
             // This depends on file semantics.
             // In compio_write, we usually only move cursor by `size`.
             // But here `ptr_bytes_written` tracks `size`.
             // If we pad, it's invisible to the user?
             // If we are at EOF, and write 1 byte, but alloc 16 bytes.
             // The file size becomes +16 (block aligned).
             // But cursor becomes +1.
             // Next write at +1.
             // But we allocated block [0, 16).
             // Next write at 1 will overwrite data in block [0, 16).
             
             // So cursor moves by `copy_from_ptr`.
             // But `current_pos` for next iteration?
             // `current_pos` is derived from `write_start + ptr_bytes_written`.
             // So if we padded, `current_pos` does NOT advance over padding.
             // Next iteration starts at `write_start + ptr_bytes_written`.
             // Which is inside the block we just created?
             
             // Wait. If we created a block [0, 16) but only wrote 1 byte.
             // `ptr_bytes_written` = 1.
             // Next iter: `current_pos` = 1.
             // We look for block at 1.
             // `range` does NOT contain the new block (it was fetched at start).
             // So we think it's a gap?
             // We create ANOTHER block at 1?
             // NO!
             
             // The loop relies on `range` being up to date OR covering the whole write.
             // But `range` is stale as we add blocks.
             // If `COMPIO_DISABLE_INSERT_ERASE`, blocks are fixed size.
             // So if we pad, it implies we are done (remaining < block_size).
             // So loop terminates.
             
             // BUT in dynamic mode (no macro).
             // If we filled a gap partially?
             // `available_space` logic handles partial fill.
             // `copy_from_ptr` == `current_block_size`.
             // `pad_size` == 0.
             // So `ptr_bytes_written` advances by `current_block_size`.
             // `current_pos` advances by `current_block_size`.
             // We are at end of new block.
             
             // The ONLY case where `pad_size > 0` is if `remaining_write < current_block_size`.
             // This implies `remaining_write` is small.
             // This happens at the very end of `compio_write`.
             // So loop will terminate after this.
             
             // So `cursor` advancing by `copy_from_ptr` is correct.
             // And we don't need to worry about overlapping next iteration because there is no next iteration.
             
             // EXCEPT: `file->size` update.
             // `file->size = std::max(file->cursor, file->size)`.
             // If we padded, `file->size` should probably include padding?
             // If we allocated a block, the file size PHYSICALLY increased.
             // But logically `file->size` is the logical size (max written byte).
             // In `compio`, `file->size` tracks logical size?
             // Actually `header->file_size` tracks physical size of archive.
             // `file->size` tracks logical size of the user file.
             // If we pad, does logical size increase?
             // Usually no.
             // BUT `file_table_item->size`?
             
             // In `compio_write_impl` original:
             // `file_table_item->size = file->size += left_pad + copy_size;`
             // It included padding!
             
             // So `file->size` should include padding?
             // If so, `cursor` should also advance?
             // No, cursor is where next write happens.
             
             // Wait, if I write 1 byte, and file grows by 16.
             // If `file->size` becomes 16.
             // And `cursor` becomes 1.
             // Next write at 1.
             // It sees block [0, 16).
             // It writes at 1.
             // This is fine.
             
             // So `file->size` should take `current_block_size`.
             // `file->size = std::max(file->size, current_pos + current_block_size)`.
             
             file_table_item->size = file->size = std::max(file->size, current_pos + current_block_size);
             
             // But `file->cursor` only advances by real bytes written?
             // `compio_write` returns `size`.
             // User expects cursor += size.
             // So `file->cursor += copy_from_ptr` (if copy_from_ptr == remaining).
             
             // Wait, if `pad_size > 0`, then `copy_from_ptr` IS `remaining_write`.
             // So `ptr_bytes_written` becomes `size`.
             // Loop ends.
             
             // So `file->cursor` logic is: `file->cursor = write_start + ptr_bytes_written`.
             // So `file->cursor += copy_from_ptr` is correct.
             
        }
    }

    block_reader->disable_temporary_index();

    file->cached_leaf = smart_infile_object<compio::index_node>();

    validate_tree(archive->index, file);

    if (!wal_tx_guard.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return ptr_bytes_written;
    }

    // Auto-batching logic - end batch if threshold reached
    end_auto_batch_if_needed(file);

    // Invalidate range cache after successful write operations
    invalidate_range_cache(file);

    return size;
}

uint64_t compio_write(const void *ptr, uint64_t size, compio_file *file) {
    if (!file || !file->archive) {
        return 0;
    }
    std::unique_lock<std::shared_mutex> lock(file->archive->mutex);
    return compio_write_impl(ptr, size, file);
}

uint64_t compio_read(void *ptr, uint64_t size, compio_file *file) {
    if (!file || !file->archive) {
        return 0;
    }

    DEBUG_PRINT("\ncompio_read(cursor=%" PRIu64 ", size=%" PRIu64 ", file_size=%" PRIu64 ")\n", file->cursor, size, file->size);
    std::shared_lock<std::shared_mutex> lock(file->archive->mutex);

    const auto *archive = file->archive;
    const auto block_reader = archive->block_reader;

    auto file_table_item = readonly(archive->header, header)->ftable.find(file->name);
    if (!file_table_item) {
        WARNING_PRINT("warning: no such file in header.ftable\n");
        errno = ENOENT;
        return 0;
    }

    if (file->cursor >= file->size) {
        return 0;
    }
    size = std::min(size, file->size - file->cursor);

    if (size == 0) {
        return 0;
    }

    const uint64_t read_start = file->cursor;
    const uint64_t read_end = read_start + size;

    auto p_ptr = reinterpret_cast<uint8_t *>(ptr);
    uint64_t ptr_bytes_read = 0;

    // enable temporary index, so it will fix expired tree_vals
    block_reader->enable_temporary_index();

    bool retried_global_cache = false;

    while (file->cursor < read_end) {
        // fprintf(stderr, "compio_read: cursor=%" PRIu64 ", read_end=%" PRIu64 "\n", file->cursor, read_end);
        const tree_key search_key = {file->hash, file->cursor};
        
        int block_idx = -1;
        bool used_cached_node = false;

        // Try cached node first
        if (file->cached_leaf) {
            const auto& node = *readonly(file->cached_leaf, index_node);
            auto it = std::upper_bound(node.keys.begin(), node.keys.end(), search_key);
            if (it != node.keys.begin()) {
                size_t idx = std::distance(node.keys.begin(), it) - 1;
                const auto& k = node.keys[idx];
                const auto& v = node.values[idx];
                if (k.hash == file->hash && k.pos <= file->cursor && k.pos + v.size > file->cursor) {
                    block_idx = idx;
                    used_cached_node = true;
                }
            }
        }
        
        // Cache miss?
        if (block_idx == -1) {
             file->cached_leaf = archive->index->find_node(search_key);
             if (!file->cached_leaf) {
                  // Should not happen if root exists, unless empty tree
                  // WARNING_PRINT("error: failed to find node for key\n");
                  // break; 
             } else {
                 const auto& node = *readonly(file->cached_leaf, index_node);
                 auto it = std::upper_bound(node.keys.begin(), node.keys.end(), search_key);
                 if (it != node.keys.begin()) {
                    size_t idx = std::distance(node.keys.begin(), it) - 1;
                    const auto& k = node.keys[idx];
                    const auto& v = node.values[idx];
                    if (k.hash == file->hash && k.pos <= file->cursor && k.pos + v.size > file->cursor) {
                        block_idx = idx;
                    }
                 }
             }
        }
        
        if (block_idx == -1) {
            // Check if we missed a block that spans across node boundary (or cache miss)
            auto overlapping = archive->index->get_block(search_key);
            if (overlapping) {
                const auto& key = overlapping->first;
                const auto& val = overlapping->second;
                const std::shared_ptr<const block> b = block_reader->read_block(val.addr, key);
                
                if (!b) {
                    if (!retried_global_cache && archive->index) {
                         // WARNING_PRINT("warning: read_block failed with fresh node (via get_block). Clearing global index cache and retrying.\n");
                         archive->index->clear_cache();
                         retried_global_cache = true;
                         // Also clear local cache just in case
                         file->cached_leaf = smart_infile_object<compio::index_node>();
                         continue;
                    }

                    // failed to decompress OR checksum mismatch
                    WARNING_PRINT("warning: failed to read block at addr=%" PRIu64 " (corruption or decompression error)\n", val.addr);
                    errno = EIO;
                    break;
                }
                
                assert(b->size() == val.size);

                const uint64_t block_start = key.pos;
                const uint64_t block_end = key.pos + b->size();
                const uint64_t copy_end = std::min(read_end, block_end); // Clamped by read_end
                
                // We always copy starting from current cursor
                const uint64_t copy_size = copy_end - file->cursor;
                const uint64_t dec_offset = file->cursor - block_start;
                
                DEBUG_PRINT("[CR]copying data of size %" PRIu64 " from block (offset=%" PRIu64 ") (via get_block)\n", copy_size,
                            dec_offset);
                assert(dec_offset + copy_size <= b->size());

                std::copy_n(b->data() + dec_offset, copy_size, p_ptr);
                p_ptr += copy_size;
                ptr_bytes_read += copy_size;
                file->cursor += copy_size;
                
                // Don't disable temporary index, because loop continues
                // In fact, we should disable it at loop end (which is outside loop)
                continue;
            }

             // GAP or EOF
             if (file->cached_leaf) {
                  // debug print removed
             }
             const auto& node = *readonly(file->cached_leaf, index_node);
             auto it = std::upper_bound(node.keys.begin(), node.keys.end(), search_key);
             
             uint64_t gap_end = read_end;
             if (it != node.keys.end() && it->hash == file->hash) {
                 gap_end = std::min(gap_end, it->pos);
             } else {
                 // Check if there is a block in the next node
                 const tree_key k_min = {file->hash, file->cursor};
                 const tree_key k_max = {file->hash, read_end};
                 auto range_opt = archive->index->get_range(k_min, k_max);
                 if (range_opt && !range_opt->empty()) {
                      gap_end = std::min(gap_end, range_opt->front().first.pos);
                 }
             }
             
             const uint64_t gap_size = gap_end - file->cursor;
             if (gap_size > 0) {
                 DEBUG_PRINT("[CR]filling gap of size %" PRIu64 "\n", gap_size);
                 std::fill_n(p_ptr, gap_size, 0);
                 p_ptr += gap_size;
                 ptr_bytes_read += gap_size;
                 file->cursor += gap_size;
                 continue;
             } else {
                 // Should not happen if logic is correct
                 WARNING_PRINT("warning: read hit gap or end of blocks (gap_size=0)\n");
                 break;
             }
        }

        const auto& node = *readonly(file->cached_leaf, index_node);
        const auto& key = node.keys[block_idx];
        const auto& val = node.values[block_idx];

        const std::shared_ptr<const block> b = block_reader->read_block(val.addr, key);
        if (!b) {
            if (used_cached_node) {
                 // Stale cache? Retry.
                 file->cached_leaf = smart_infile_object<compio::index_node>();
                 continue;
            }

            if (!retried_global_cache && archive->index) {
                 // WARNING_PRINT("warning: read_block failed with fresh node. Clearing global index cache and retrying.\n");
                 archive->index->clear_cache();
                 retried_global_cache = true;
                 
                 // Also clear local cache just in case
                 file->cached_leaf = smart_infile_object<compio::index_node>();
                 continue;
            }

            // failed to decompress OR checksum mismatch
            WARNING_PRINT("warning: failed to read block at addr=%" PRIu64 " (corruption or decompression error)\n", val.addr);
            errno = EIO;
            break;
        }
        
        assert(b->size() == val.size);

        const uint64_t block_start = key.pos;
        const uint64_t block_end = key.pos + b->size();
        const uint64_t copy_end = std::min(read_end, block_end); // Clamped by read_end
        
        // We always copy starting from current cursor
        const uint64_t copy_size = copy_end - file->cursor;
        const uint64_t dec_offset = file->cursor - block_start;
        
        DEBUG_PRINT("[CR]copying data of size %" PRIu64 " from block (offset=%" PRIu64 ")\n", copy_size,
                    dec_offset);
        assert(dec_offset + copy_size <= b->size());

        std::copy_n(b->data() + dec_offset, copy_size, p_ptr);
        p_ptr += copy_size;
        ptr_bytes_read += copy_size;
        file->cursor += copy_size;
    }

    block_reader->disable_temporary_index();
    
    // validate_tree is expensive and redundant here
    // validate_tree(archive->index, file);

    return ptr_bytes_read;
}

uint64_t compio_insert(const void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_insert(cursor=%" PRIu64 ", size=%" PRIu64 ")\n", file->cursor, size);

    if (!file || !file->archive) {
        return 0;
    }
    std::unique_lock<std::shared_mutex> lock(file->archive->mutex);

#ifdef COMPIO_DISABLE_INSERT_ERASE
    WARNING_PRINT("warning: insert operations are disabled (COMPIO_DISABLE_INSERT_ERASE)\n");
    return 0;
#endif

    // behave the same as compio_write, when inserting after file end
    if (file->cursor >= file->size) {
        return compio_write_impl(ptr, size, file);
    }

    auto *archive = file->archive;
    const auto block_reader = archive->block_reader;

    if ((archive->mode_b & mode_bit::r) && !(archive->mode_b & mode_bit::plus)) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

    if (size == 0) {
        return 0;
    }

    // Auto-batching logic - start batching if sequential
    start_auto_batch_if_needed(file, file->cursor, size);

    auto file_table_item = archive->header->ftable.find(file->name);
    if (!file_table_item) {
        // should not happen, because compio_open_file creates ftable record
        WARNING_PRINT("warning: no such file in header.ftable\n");
        errno = ENOENT;
        return 0;
    }

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    WalTransactionGuard wal_tx_guard(wal_ptr);

    const tree_key cursor_key = {file->hash, file->cursor};
    const auto key_val = archive->index->get_block(cursor_key);
    const uint64_t block_size = archive->config.block_size;
    const uint64_t block_size__minimum = archive->config.block_size__minimum;
    const uint64_t block_size__maximum = archive->config.block_size__maximum;
    const tree_key file_end_key{file->hash, UINT64_MAX};

    bool inserted_into_existing = false;
    if (key_val.has_value()) {
        const auto &[left_key, left_val] = key_val.value();
        const auto left_b = block_reader->read_block(left_val.addr, left_key);
        if (!left_b) {
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            return 0;
        }
        assert(left_b->size() == left_val.size);

        assert(left_key.pos <= file->cursor);
        const uint64_t left_size = file->cursor - left_key.pos;
        const uint64_t old_size = left_b->size();
        const uint64_t right_size = old_size - left_size;
        const bool is_inside_block = (left_size > 0 && left_size < old_size);

        if (is_inside_block && old_size + size <= block_size__maximum) {
            const tree_key old_block_end_key{file->hash, left_key.pos + old_size};
            archive->index->add_to_range(size, old_block_end_key, file_end_key);
            block_reader->add_to_range(size, old_block_end_key, file_end_key);

            left_b->grow(old_size + size);
            if (right_size > 0) {
                std::copy_backward(left_b->data() + left_size, left_b->data() + old_size,
                                   left_b->data() + old_size + size);
            }
            std::copy_n(reinterpret_cast<const uint8_t *>(ptr), size, left_b->data() + left_size);

            file->cursor += size;
            file->size += size;
            file_table_item->size = file->size;
            inserted_into_existing = true;
        } else if (is_inside_block) {
            const tree_key old_block_end_key{file->hash, left_key.pos + old_size};
            archive->index->add_to_range(size, old_block_end_key, file_end_key);
            block_reader->add_to_range(size, old_block_end_key, file_end_key);

            const auto left_b_const = std::static_pointer_cast<const block>(left_b);
            const uint8_t *left_data = left_b_const->data();
            const uint8_t *insert_data = reinterpret_cast<const uint8_t *>(ptr);
            block_reader->remove_block(left_b);

            struct segment {
                const uint8_t *data;
                uint64_t size;
            };
            std::vector<segment> segments;
            segments.reserve(3);
            if (left_size > 0) {
                segments.push_back({left_data, left_size});
            }
            segments.push_back({insert_data, size});
            if (right_size > 0) {
                segments.push_back({left_data + left_size, right_size});
            }

            std::size_t segment_index = 0;
            uint64_t segment_offset = 0;
            auto copy_sequence = [&](uint8_t *dst, uint64_t bytes_needed) {
                uint64_t copied = 0;
                while (copied < bytes_needed) {
                    assert(segment_index < segments.size());
                    const auto &seg = segments[segment_index];
                    const uint64_t seg_remaining = seg.size - segment_offset;
                    const uint64_t take = std::min<uint64_t>(seg_remaining, bytes_needed - copied);
                    std::copy_n(seg.data + segment_offset, take, dst + copied);
                    copied += take;
                    segment_offset += take;
                    if (segment_offset == seg.size) {
                        segment_offset = 0;
                        ++segment_index;
                    }
                }
            };

            uint64_t bytes_left = old_size + size;
            uint64_t block_cursor = left_key.pos;
            while (bytes_left > 0) {
                uint64_t current_block_size;
                if (bytes_left < block_size || bytes_left - block_size < block_size__minimum) {
                    current_block_size = bytes_left;
                } else {
                    current_block_size = block_size;
                }

                const tree_key key{file->hash, block_cursor};
                const auto b = block_reader->create_block(current_block_size, key);
                copy_sequence(b->data(), current_block_size);

                block_cursor += current_block_size;
                bytes_left -= current_block_size;
            }

            file->cursor += size;
            file->size += size;
            file_table_item->size = file->size;
            inserted_into_existing = true;
        }
    }

    if (!inserted_into_existing) {
        std::shared_ptr<block> right_b;

        if (key_val.has_value()) {
            const auto &[left_key, left_val] = key_val.value();
            const auto left_b = block_reader->read_block(left_val.addr, left_key);
            if (!left_b) {
                WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
                errno = EIO;
                return 0;
            }
            assert(left_b->size() == left_val.size);

            assert(left_key.pos <= file->cursor);
            const uint64_t left_size = file->cursor - left_key.pos;
            if (left_size > 0 && left_size < left_b->size()) {
                assert(left_key.pos + left_b->size() > file->cursor);
                const uint64_t right_size = left_b->size() - left_size;
                right_b = block_reader->create_block(right_size, cursor_key);
                if (right_size > 0) {
                    std::copy(left_b->data() + left_size, left_b->data() + left_size + right_size,
                              right_b->data());
                }
                left_b->shrink(left_size);
            }
        }

        archive->index->add_to_range(size, cursor_key, file_end_key);
        block_reader->add_to_range(size, cursor_key, file_end_key);

        if (right_b && right_b->key() == cursor_key) {
            tree_key new_key = cursor_key;
            new_key.pos += size;
            right_b->set_key(new_key);
        }

        auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);
        uint64_t total_bytes_left = size;
        uint64_t current_cursor = file->cursor;

        while (total_bytes_left > 0) {
            uint64_t current_block_size;
            if (total_bytes_left < block_size || total_bytes_left - block_size < block_size__minimum) {
                current_block_size = total_bytes_left;
            } else {
                current_block_size = block_size;
            }

            const tree_key key{file->hash, current_cursor};
            const auto b = block_reader->create_block(current_block_size, key);
            std::copy_n(p_ptr, current_block_size, b->data());

            p_ptr += current_block_size;
            current_cursor += current_block_size;
            file->size += current_block_size;
            file_table_item->size += current_block_size;
            total_bytes_left -= current_block_size;
        }

        file->cursor = current_cursor;
    }
    
    file->cached_leaf = smart_infile_object<compio::index_node>();
    invalidate_range_cache(file);

    validate_tree(archive->index, file);

    if (!wal_tx_guard.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return 0;
    }

    // Auto-batching logic - end batch if threshold reached
    end_auto_batch_if_needed(file);

    return size;
}

uint64_t compio_erase(uint64_t size, compio_file *file) {
    if (!file || !file->archive) {
        return 0;
    }
    DEBUG_PRINT("\ncompio_erase(cursor=%" PRIu64 ", size=%" PRIu64 ")\n", file->cursor, size);

    std::unique_lock<std::shared_mutex> lock(file->archive->mutex);

#ifdef COMPIO_DISABLE_INSERT_ERASE
    WARNING_PRINT("warning: erase operations are disabled (COMPIO_DISABLE_INSERT_ERASE)\n");
    return 0;
#endif

    auto *archive = file->archive;
    const auto block_reader = archive->block_reader;

    if ((archive->mode_b & mode_bit::r) && !(archive->mode_b & mode_bit::plus)) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

    // Auto-batching logic - start batching if sequential
    start_auto_batch_if_needed(file, file->cursor, size);

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    WalTransactionGuard wal_tx_guard(wal_ptr);

    if (file->cursor > file->size) {
        return 0;
    }

    size = std::max(UINT64_C(0), std::min(size, file->size - file->cursor));
    if (size == 0) {
        return 0;
    }

    auto file_table_item = archive->header->ftable.find(file->name);
    if (!file_table_item) {
        // should not happen, because compio_open_file creates ftable record
        WARNING_PRINT("warning: no such file in header.ftable\n");
        errno = ENOENT;
        return 0;
    }

    const uint64_t erase_start = file->cursor;
    const uint64_t erase_end = erase_start + size;

    const tree_key key_min = {file->hash, erase_start};
    const tree_key key_max = {file->hash, erase_end};
    auto range_opt = archive->index->get_range(key_min, key_max);
    if (!range_opt) {
        WARNING_PRINT("error: failed to read index range during insert/erase\n");
        errno = EIO;
        return 0;
    }
    const auto& range = *range_opt;
    assert(!range.empty());
    assert(range.front().first.pos <= erase_start);
    assert(range.back().first.pos + range.back().second.size >= erase_end);
    validate_no_gaps_in_range(range);

    uint64_t bytes_erased = 0;
    std::optional<tree_key> left_to_merge = std::nullopt;
    std::optional<tree_key> right_to_merge = std::nullopt;

    block_reader->enable_temporary_index();
    for (const auto &[key, val] : range) {
        DEBUG_PRINT("[CE]reading block ({%" PRIu64 ",%" PRIu64 "}-{%" PRIu64 ",%" PRIu64 "})\n", key.hash, key.pos, val.addr,
                    val.size);
        const auto b = block_reader->read_block(val.addr, key);
        if (!b) {
            // failed to decompress
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            block_reader->disable_temporary_index();
            return bytes_erased;
        }
        assert(b->size() == val.size);

        const uint64_t block_start = key.pos;
        const uint64_t block_end = key.pos + b->size();
        const uint64_t block_erase_end = std::min(erase_end, block_end);
        const uint64_t block_erase_start = std::max(erase_start, block_start);
        assert(block_erase_end > block_erase_start);
        const uint64_t block_erase_size = block_erase_end - block_erase_start;
        const uint64_t erase_start_offset = block_erase_start - block_start;
        const uint64_t erase_end_offset = block_erase_end - block_start;

        DEBUG_PRINT("[CE]block=(%" PRIu64 ", %" PRIu64 "), erase=(%" PRIu64 ", %" PRIu64 ") -> block_erase=(%" PRIu64 ", %" PRIu64 ")\n",
                    block_start, block_end, erase_start, erase_end, block_erase_start,
                    block_erase_end);
        const uint64_t left_size = erase_start_offset;
        const uint64_t right_size = block_end - block_erase_end;
        DEBUG_PRINT("[CE]---left_size=%" PRIu64 ", erase_size=%" PRIu64 ", right_size=%" PRIu64 "\n", left_size,
                    block_erase_size, right_size);

        if (left_size > 0 && right_size == 0) {
            left_to_merge = key;
            DEBUG_PRINT("[CE]---postmerge left block found ({%lu, %lu})\n", left_to_merge->hash,
                        left_to_merge->pos);
        } else if (left_size == 0 && right_size > 0) {
            right_to_merge = key + block_erase_size;
            DEBUG_PRINT("[CE]---postmerge right block found ({%lu, %lu})\n", right_to_merge->hash,
                        right_to_merge->pos);
        }

        // TODO: merge with adjacent block if new size is small

        if (block_erase_size < b->size()) {
            // keep block in btree, but update key.pos and val.size
            if (right_size > 0) {
                DEBUG_PRINT("[CE]---copying %" PRIu64 " bytes from offset=%" PRIu64 " to offset=%" PRIu64 "\n", right_size,
                            erase_end_offset, erase_start_offset);
                std::copy(b->data() + erase_end_offset, b->data() + b->size(),
                          b->data() + erase_start_offset);
            }
            DEBUG_PRINT("[CE]---shrinking from size=%" PRIu64 " to size=%" PRIu64 "\n", b->size(),
                        b->size() - block_erase_size);
            b->shrink(b->size() - block_erase_size);
            if (left_size == 0) {
                if (key.pos != erase_start) {
                    const int64_t local_shift = static_cast<int64_t>(erase_start) - static_cast<int64_t>(key.pos);
                    DEBUG_PRINT("[CE]---shifting block from %" PRIu64 " to %" PRIu64 " (shift=%" PRId64 ")\n",
                                key.pos, erase_start, local_shift);

                    // We need to shift this block to fill the gap at the start of the erase range.
                    // Since other blocks in the range are either removed or truncated from right,
                    // and subsequent blocks are shifted by the global erase size,
                    // this block is the only one that needs this specific shift.
                    archive->index->add_to_range(local_shift, key, key);
                    
                    tree_key new_key = key;
                    new_key.pos = erase_start;
                    block_reader->rename_block(key, new_key, b);
                } else {
                    DEBUG_PRINT("[CE]---left_size=0, block stays at %" PRIu64 ", size reduced\n", key.pos);
                }
            }
        } else {
            // remove block completely
            DEBUG_PRINT("[CE]---removing block\n");
            block_reader->remove_block(b);
        }
    }

    // shift blocks after cursor to the left
    const tree_key file_end_key{file->hash, UINT64_MAX};
    // Use logical size for shifting and file resizing, not physical bytes erased.
    // This ensures gaps are correctly collapsed.
    const int64_t shift = -static_cast<int64_t>(size);
    archive->index->add_to_range(shift, key_max, file_end_key);
    block_reader->add_to_range(shift, key_max, file_end_key);
    
    file->size -= size;
    // file_table_item was already found at start of function
    if (file_table_item) {
        file_table_item->size = file->size;
    }

    file->cached_leaf = smart_infile_object<compio::index_node>();
    invalidate_range_cache(file);

    if (right_to_merge.has_value() && left_to_merge.has_value()) {
        DEBUG_PRINT("[CE]---postmerge left={%lu, %lu}, right={%lu, %lu}, shift=%ld\n",
                    left_to_merge->hash, left_to_merge->pos, right_to_merge->hash,
                    right_to_merge->pos, shift);
        right_to_merge.value() += shift;

        const auto left_val = archive->index->get(left_to_merge.value());
        if (!left_val.has_value()) {
            WARNING_PRINT("warning: left_to_merge key ({%" PRIu64 ", %" PRIu64 "}) was not found in the tree after "
                          "erase operation\n",
                          left_to_merge->hash, left_to_merge->pos);
            goto after_merge;
        }
        const auto right_val = archive->index->get(right_to_merge.value());
        if (!right_val.has_value()) {
            WARNING_PRINT(
                "warning: right_to_merge key ({%" PRIu64 ", %" PRIu64 "}) was not found in the tree after "
                "erase operation\n",
                right_to_merge->hash, right_to_merge->pos);
            goto after_merge;
        }

        const auto block_size__minimum = static_cast<uint64_t>(archive->config.block_size__minimum);
        const auto block_size__maximum = static_cast<uint64_t>(archive->config.block_size__maximum);
        const auto left_size = left_val->size;
        const auto right_size = right_val->size;
        DEBUG_PRINT("[CE]---postmerge sizes: %lu and %lu\n", left_size, right_size);
        if (left_size >= block_size__minimum && right_size >= block_size__minimum) {
            DEBUG_PRINT("[CE]---postmerge skip\n");
            goto after_merge;
        }

        const auto left_b = block_reader->read_block(left_val->addr, left_to_merge.value());
        const auto right_b = block_reader->read_block(right_val->addr, right_to_merge.value());

        if (left_size + right_size < block_size__maximum) {
            // full merge into one block
            DEBUG_PRINT("[CE]---postmerge into one block\n");
            left_b->grow(left_b->size() + right_b->size());
            std::copy_n(right_b->data(), right_b->size(), left_b->data() + left_size);
            block_reader->remove_block(right_b);
        } else {
            // partial merge from bigger block into the smaller one
            const uint64_t new_left_size = (left_size + right_size) / 2;
            const uint64_t new_right_size = left_size + right_size - new_left_size;
            const uint64_t gap_size = (left_size > right_size) ? (left_size - new_left_size)
                                                               : (right_size - new_right_size);
            if (left_size > right_size) {
                DEBUG_PRINT("[CE]---postmerge partial from left to right\n");
                right_b->grow(new_right_size);
                std::copy_backward(right_b->data(), right_b->data() + right_size,
                                   right_b->data() + new_right_size);
                std::copy_n(left_b->data() + new_left_size, gap_size, right_b->data());
                left_b->shrink(new_left_size);
            } else {
                DEBUG_PRINT("[CE]---postmerge partial from right to left\n");
                left_b->grow(new_left_size);
                std::copy_n(right_b->data(), gap_size, left_b->data() + left_size);
                std::copy_n(right_b->data() + gap_size, new_right_size, right_b->data());
                right_b->shrink(new_right_size);
            }

            int64_t key_delta = (left_b->key().pos + left_b->size()) - right_b->key().pos;
            if (key_delta != 0) {
                DEBUG_PRINT("[CE]---postmerge shifting right block by %ld\n", key_delta);
                archive->index->add_to_range(key_delta, right_to_merge.value(), right_to_merge.value());
                block_reader->add_to_range(key_delta, right_to_merge.value(), right_to_merge.value());
            }
        }
    }
after_merge:

    validate_tree(archive->index, file, true);

    if (!wal_tx_guard.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return 0;
    }

    // Auto-batching logic - end batch if threshold reached
    end_auto_batch_if_needed(file);

    return size;
}

static void sync_files_table(compio_archive *archive) {
    if (!archive || !archive->header) return;

    // Only applies to v5 format
    if (archive->header->magic_number != COMPIO_MAGIC_NUMBER) return;

    // Copy-on-write for the files table:
    // Always allocate new space for the table and write into it, then update the
    // header to point to the new region. We intentionally do NOT deallocate the
    // old table here, because this function has no way to know when the updated
    // header has been made durable on disk.
    //
    // Note: This intentionally leaks the old table block until a "GC" or full
    // defragmentation (rebuild from index) is implemented. This is the price for
    // crash safety with the current double-buffered header design.

    uint32_t current_capacity = archive->header->ftable.max_files;
    if (current_capacity == 0) {
        return;
    }

    // Calculate needed size
    // Each entry is COMPIO_FNAME_MAX_SIZE (256) + sizeof(uint64_t) (8) = 264 bytes.
    // We allocate for full capacity.
    uint64_t needed_size = static_cast<uint64_t>(current_capacity) * (COMPIO_FNAME_MAX_SIZE + sizeof(uint64_t));

    // Allocate space using allocator if available
    uint64_t new_addr = 0;
    if (archive->allocator) {
         // We allocate raw space. This is not a "block" with compression.
         // It is metadata.
         new_addr = archive->allocator->allocate(needed_size);
    } else {
         // Fallback if no allocator
         // Append to end of file
         if (fseek64(archive->file, 0, SEEK_END) == 0) {
              new_addr = ftell64(archive->file);
              // Update header file_size manually if appending blindly
              archive->header->file_size = std::max(archive->header->file_size, new_addr + needed_size);
         }
    }
    
    if (new_addr != 0 && new_addr != UINT64_MAX) {
        // Update header to point to the newly allocated table.
        // We do NOT free the old address.
        archive->header->files_table_addr = new_addr;
        archive->header->files_table_capacity = current_capacity;
        
        // Write the table content at the new address.
        archive->header->ftable.write_to(archive->file, archive->header->files_table_addr);
    } else {
         WARNING_PRINT("warning: failed to allocate space for files table\n");
    }
}

void compio_flush(compio_archive *archive) {
    if (!archive) return;
    std::unique_lock<std::shared_mutex> lock(archive->mutex);
    
    // Check if we have write permission (w, a, or + modes)
    // mode_bit::r is set for 'r' and 'r+'. 'w'/'a' don't set 'r'.
    // So we need explicit check for write capability.
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);

    // Start atomic transaction for the entire flush operation
    // Pass nullptr if we shouldn't use WAL
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    
    if (wal_ptr && wal_ptr->get_batch_depth() == 0) {
        wal_ptr->begin_transaction();
    }
    
    bool wal_active = (wal_ptr != nullptr);

    // Track whether all durability operations succeed; used to decide if we can safely checkpoint.
    bool durable = true;

    if (archive->block_reader) archive->block_reader->clear_cache();
    if (archive->block_reader) archive->block_reader->invalidate_temporary_index();
    if (archive->index) archive->index->clear_cache();

    // Sync files table (allocate if needed, write to disk)
    if (can_write) {
        sync_files_table(archive);
    }

    // Save allocator state (updates header fields)
    if (archive->allocator && can_write) {
         archive->allocator->save_state(archive);
    }
    
    // Double-buffered Header Write
    flush_header_double_buffered(archive);
    
    // Commit transaction (this performs WAL write/flush and optionally fsync based on sync mode)
    if (wal_active && wal_ptr->get_batch_depth() == 0) {
        if (!wal_ptr->commit_transaction(archive->file, archive->config.wal_max_size_bytes)) {
            WARNING_PRINT("warning: WAL commit failed in compio_flush\n");
            durable = false;
        }
    }

    if (fflush(archive->file)) {
        WARNING_PRINT("warning: fflush failed\n");
        durable = false;
    }
    
    // Now that everything is flushed to the OS buffer for the main file,
    // and the WAL transaction is committed and synced (via commit_transaction),
    // we can safely checkpoint, but only if all durability steps have succeeded.
    //
    // Checkpointing means:
    // 1. fsync the main archive file (ensure data is durable).
    // 2. Truncate the WAL (it is no longer needed since main file is up to date).
    
    if (wal_active && durable) {
        bool main_file_synced = true;
#ifdef _WIN32
        if (_commit(_fileno(archive->file)) != 0) {
            WARNING_PRINT("warning: _commit failed in compio_flush checkpoint\n");
            main_file_synced = false;
        }
#else
        if (fsync(fileno(archive->file)) != 0) {
            WARNING_PRINT("warning: fsync failed in compio_flush checkpoint\n");
            main_file_synced = false;
        }
#endif
        if (!main_file_synced) {
            WARNING_PRINT("warning: skipping WAL checkpoint due to main file sync failure\n");
        } else {
            if (!archive->wal->checkpoint()) {
                 WARNING_PRINT("warning: WAL checkpoint failed\n");
            }
        }
    } else if (wal_active && archive->wal && !durable) {
        // We had a durability failure earlier (e.g., WAL commit or fflush);
        // do not truncate the WAL so that recovery remains possible.
        WARNING_PRINT("warning: skipping WAL checkpoint due to earlier durability failure\n");
    }
}

int compio_repair(const char *path, const char *output_dir) {
    // Basic null checks
    if (!path) {
        WARNING_PRINT("error: path is null\n");
        return COMPIO_ERROR;
    }
    if (!output_dir) {
        WARNING_PRINT("error: output_dir is null\n");
        return COMPIO_ERROR;
    }

    // Wrap filesystem operations to prevent C++ exceptions escaping C API
    try {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            WARNING_PRINT("error: file not found: %s\n", path);
            return COMPIO_ERROR;
        }

        fs::path out_dir_path(output_dir);
        // Create output directory if it doesn't exist
        if (!fs::exists(out_dir_path, ec)) {
            if (!fs::create_directories(out_dir_path, ec)) {
                 WARNING_PRINT("error: failed to create output directory: %s\n", output_dir);
                 return COMPIO_ERROR;
            }
        } else if (!fs::is_directory(out_dir_path, ec)) {
            WARNING_PRINT("error: output path exists but is not a directory: %s\n", output_dir);
            return COMPIO_ERROR;
        }

        FILE *f_raw = fopen(path, "rb");
        if (!f_raw) {
            WARNING_PRINT("error: failed to open file: %s (errno=%d)\n", path, errno);
            return COMPIO_ERROR;
        }
        
        // Use RAII to ensure file is closed even if exceptions occur
        std::unique_ptr<FILE, void(*)(FILE*)> f_guard(f_raw, [](FILE* f){ fclose(f); });
        FILE* f = f_raw;

        fseek64(f, 0, SEEK_END);
        uint64_t file_size = ftell64(f);
        fseek64(f, 0, SEEK_SET);

        if (file_size < sizeof(header)) {
            WARNING_PRINT("error: file too small to contain header\n");
            // f_guard will close f
            return COMPIO_ERROR;
        }

        header h;
        bool valid_header = h.load_and_validate(f, 0);
        if (!valid_header) {
            WARNING_PRINT("warning: header corrupted, using default settings for salvage (degree=16, lz4, 16KB blocks)\n");
            h.b_tree_degree = 16;
            h.compression_type = COMPIO_COMPRESS_LZ4;
            h.block_size = 16384;  // Match current default (16KB)
            // Clear files table to avoid undefined behavior from iterating uninitialized data
            h.ftable.files.clear();
            h.ftable.n_files = 0;
            h.ftable.max_files = 0;
        } else {
            WARNING_PRINT("info: header valid, degree=%u, compression=%u\n", h.b_tree_degree, h.compression_type);
        }

        std::map<uint64_t, std::string> hash_to_name;
        if (valid_header) {
            for (const auto &file : h.ftable.files) {
                if (file.name[0] != '\0') {
                    uint64_t hash = fnv1a(file.name);
                    hash_to_name[hash] = std::string(file.name);
                }
            }
        }

        struct block_meta {
            uint64_t addr;
            uint64_t size;
            uint64_t original_size;
            bool is_compressed;
        };

        std::map<uint64_t, block_meta> discovered_blocks;
        
        struct file_part {
            uint64_t pos;
            uint64_t addr;
            uint64_t size;
        };
        std::map<uint64_t, std::vector<file_part>> index_files;

        constexpr size_t BUFFER_SIZE = 1024 * 1024;
        std::vector<uint8_t> buffer(BUFFER_SIZE);
        
        uint64_t offset = 0;
        while (offset < file_size) {
            if (fseek64(f, offset, SEEK_SET) != 0) break;
            size_t bytes_read = fread(buffer.data(), 1, BUFFER_SIZE, f);
            if (bytes_read == 0) break;

            for (size_t i = 0; i < bytes_read; ++i) {
                uint64_t current_addr = offset + i;
                uint8_t sig = buffer[i];

                if (sig == index_node::signature) {
                    uint64_t saved_pos = ftell64(f);
                    index_node node(h.b_tree_degree);
                    if (node.read_from(f, current_addr)) {
                         for (size_t k = 0; k < node.keys.size(); ++k) {
                            if (k < node.values.size()) {
                                index_files[node.keys[k].hash].push_back({
                                    node.keys[k].pos, 
                                    node.values[k].addr, 
                                    node.values[k].size
                                });
                            }
                        }
                    }
                    fseek64(f, saved_pos, SEEK_SET);
                }
                
                if (sig == storage_block::signature || sig == storage_block::signature_crc32c) {
                    uint64_t saved_pos = ftell64(f);
                    storage_block sb;
                    if (sb.read_from(f, current_addr)) {
                        discovered_blocks[current_addr] = {
                            current_addr, 
                            sb.size, 
                            sb.original_size, 
                            (bool)sb.is_compressed
                        };
                    }
                    fseek64(f, saved_pos, SEEK_SET);
                }
            }
            offset += bytes_read;
        }

        compio_compressor compressor;
        compio_build_compressor_by_type(&compressor, (compio_compression_type)h.compression_type);

        int recovered_count = 0;
        std::set<uint64_t> claimed_addrs;

        auto dump_block = [&](FILE* out_f, uint64_t addr, uint64_t file_pos) {
            // Read block again to get data
            storage_block sb;
            if (sb.read_from(f, addr)) {
                 if (sb.is_compressed) {
                    uint64_t decomp_size = sb.original_size;
                    if (decomp_size > 1024 * 1024 * 1024) {
                         WARNING_PRINT("warning: skipping huge block decompression %" PRIu64 "\n", addr);
                         return;
                    }
                    std::vector<uint8_t> decomp_buf(decomp_size);
                    if (compressor.decompress(&compressor, decomp_buf.data(), &decomp_size, sb.data.get(), sb.size) == 0) {
                        fseek64(out_f, file_pos, SEEK_SET);
                        fwrite(decomp_buf.data(), 1, decomp_size, out_f);
                    } else {
                         WARNING_PRINT("warning: decompression failed for block at %" PRIu64 "\n", addr);
                    }
                } else {
                    fseek64(out_f, file_pos, SEEK_SET);
                    fwrite(sb.data.get(), 1, sb.size, out_f);
                }
            }
        };

        for (auto& [hash, parts] : index_files) {
            std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) {
                return a.pos < b.pos;
            });

            std::string filename;
            if (hash_to_name.count(hash)) {
                filename = hash_to_name[hash];
            } else {
                filename = "file_" + std::to_string(hash);
            }

            fs::path p(filename);
            // Sanitize filename: use only the filename component to prevent directory traversal
            std::string safe_name = p.filename().string();
            if (safe_name.empty() || safe_name == "." || safe_name == "..") {
                safe_name = "file_" + std::to_string(hash);
            }
            
            fs::path out_path = out_dir_path / safe_name;
            
            // Resolve to absolute path and check if it is within output_dir
            // Note: weakly_canonical requires file to exist, so we check parent dir
            // Simpler check: ensure out_path starts with out_dir_path
            // But out_dir_path might be relative.
            // Since we constructed out_path using operator/, and safe_name is just a filename,
            // it should be safe unless out_dir_path itself is malicious (which is user input).
            
#ifdef _WIN32
            FILE *out_f_raw = _wfopen(out_path.c_str(), L"wb");
#else
            FILE *out_f_raw = fopen(out_path.c_str(), "wb");
#endif
            if (!out_f_raw) {
                WARNING_PRINT("error: failed to create output file: %s (errno=%d)\n", out_path.string().c_str(), errno);
                continue;
            }
            std::unique_ptr<FILE, void(*)(FILE*)> out_f_guard(out_f_raw, [](FILE* f){ fclose(f); });
            FILE* out_f = out_f_raw;

            for (const auto& part : parts) {
                    if (discovered_blocks.count(part.addr)) {
                    claimed_addrs.insert(part.addr);
                    const auto& meta = discovered_blocks[part.addr];
                    dump_block(out_f, meta.addr, part.pos);
                } else {
                    storage_block sb;
                    if (sb.read_from(f, part.addr)) {
                        claimed_addrs.insert(part.addr);
                        dump_block(out_f, part.addr, part.pos);
                    }
                }
            }
            // fclose(out_f) handled by guard
            recovered_count++;
        }

        for (const auto& [addr, meta] : discovered_blocks) {
            if (claimed_addrs.find(addr) == claimed_addrs.end()) {
                 std::string safe_name = "orphan_" + std::to_string(addr) + ".bin";
                 fs::path out_path = out_dir_path / safe_name;
                 
#ifdef _WIN32
                 FILE *out_f_raw = _wfopen(out_path.c_str(), L"wb");
#else
                 FILE *out_f_raw = fopen(out_path.c_str(), "wb");
#endif
                 if (out_f_raw) {
                     std::unique_ptr<FILE, void(*)(FILE*)> out_f_guard(out_f_raw, [](FILE* f){ fclose(f); });
                     FILE* out_f = out_f_raw;
                     dump_block(out_f, meta.addr, 0);
                     // fclose(out_f) handled by guard
                     recovered_count++;
                 } else {
                     WARNING_PRINT("warning: failed to create orphan file: %s (errno=%d)\n", out_path.string().c_str(), errno);
                 }
            }
        }

        // f_guard will close f automatically
        return recovered_count;
    } catch (const std::exception& e) {
        WARNING_PRINT("error: exception during repair: %s\n", e.what());
        return COMPIO_ERROR;
    } catch (...) {
        WARNING_PRINT("error: unknown exception during repair\n");
        return COMPIO_ERROR;
    }
}

// Auto-batching accessor functions for testing

int compio_is_auto_batching(compio_file *file) {
    if (!file || !file->archive) return 0;
    std::shared_lock<std::shared_mutex> lock(file->archive->mutex);
    return file->is_auto_batching ? 1 : 0;
}

int compio_get_auto_batch_count(compio_file *file) {
    if (!file || !file->archive) return 0;
    std::shared_lock<std::shared_mutex> lock(file->archive->mutex);
    return file->auto_batch_count;
}
