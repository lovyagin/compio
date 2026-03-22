#include "compio.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <memory>
#include <utility>
#include <cerrno>

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

using namespace compio;

void compio_build_default_config(compio_config *result) {
    result->b_tree_degree = 16;
    compio_build_zlib_compressor(&result->compressor);
#ifdef NDEBUG
    result->fill_holes_with_zeros = false;
#else
    result->fill_holes_with_zeros = true;
#endif
    result->block_size = 1 << 12;
    result->block_size__minimum = 1 << 9;
    result->block_size__maximum = 1 << 14;
    result->cache_size__nodes = 128;
    result->cache_size__blocks = 16;
    result->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    result->fragmentation_threshold = 30;
    result->max_files = COMPIO_MAX_FILES;
    result->wal_max_size_bytes = 64 * 1024 * 1024; // 64 MB
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

bool compio_archive::is_readonly() const { return mode_b & mode_bit::r; }

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

    // if w+ passed as mode, we have to clear file contents (using w+)
    // otherwise we open with a+ mode to read and write
    char archive_open_mode[5];
    int mode_idx = 0;
    
    // Fix: Force update (+) mode for 'w' and 'a' to allow internal reads (e.g. reading header in 'a' mode)
    // This matches previous behavior where 'w'/'a' implied 'w+'/'a+' capability for the library.
    bool force_plus = (mode_b & mode_bit::w) || (mode_b & mode_bit::a);

    if (mode_b & mode_bit::w) {
        archive_open_mode[mode_idx++] = 'w';
    } else if (mode_b & mode_bit::a) {
        archive_open_mode[mode_idx++] = 'a';
    } else {
        archive_open_mode[mode_idx++] = 'r';
    }
    
    archive_open_mode[mode_idx++] = 'b';
    
    if ((mode_b & mode_bit::plus) || force_plus) {
        archive_open_mode[mode_idx++] = '+';
    }
    
    archive_open_mode[mode_idx] = '\0';

    FILE *file;
    file = fopen(fp, archive_open_mode);
    if (file == nullptr) {
        return NULL;
    }

    // Initialize WAL Manager
    auto wal = std::make_unique<compio::WalManager>(fp);
    
    // Check for recovery (only if we are not creating a new file from scratch with "w")
    if (!(mode_b & mode_bit::w)) {
        if (wal->has_pending_recovery()) {
            // Need read-write access to file for recovery
            // We use the derived force_plus logic or explicit plus
            bool can_write = (mode_b & mode_bit::plus) || (mode_b & mode_bit::a) || force_plus;
            
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
        if (c->max_files == 0) {
             const_cast<compio_config&>(archive->config).max_files = hdr.ftable.max_files;
        } else if (hdr.ftable.max_files != static_cast<uint32_t>(c->max_files)) {
            errno = EINVAL;
            WARNING_PRINT("warning: max_files mismatch while opening archive (file=%u, config=%d)\n",
                          hdr.ftable.max_files, c->max_files);
            goto no_allocator;
        }
    }

    // initialize allocator before btree, because btree uses allocator for creating root node
    archive->allocator = new compio::block_allocator(archive, archive->wal.get());
    if (!archive->allocator) {
        WARNING_PRINT("warning: failed to allocate memory for allocator\n");
        goto no_allocator;
    }

    archive->index = new btree(c->b_tree_degree, mode_b & mode_bit::r, archive->header,
                               archive->allocator, file, c->cache_size__nodes, &archive->io_mutex, archive->wal.get());
    if (!archive->index) {
        WARNING_PRINT("warning: failed to allocate memory for btree\n");
        goto no_index;
    }

    archive->block_reader = new compio::storage_block_reader(
        file, archive->allocator, archive->index,
        &archive->config.compressor, archive->config.cache_size__blocks, &archive->io_mutex, archive->wal.get());
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
        if (!(archive->mode_b & mode_bit::r)) {
            file_table_item = archive->header->ftable.add(name);
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
        auto all_blocks = archive->index->get_range(key_min, key_max);

        // Save current file position
        int64_t saved_pos = ftell64(archive->file);

        // Deallocate all blocks and remove them from index
        for (const auto &[key, val] : all_blocks) {
            if (val.addr != 0) {
                // Read storage_block metadata to get compressed size
                storage_block sb;
                sb.read_from(archive->file, val.addr);

                // Deallocate: metadata + compressed data size
                archive->allocator->deallocate(val.addr, STORAGE_BLOCK_METASIZE + sb.size);
            }

            // Remove block from B-tree index
            archive->index->remove(key);
        }

        // Restore file position
        if (saved_pos >= 0) {
            fseek64(archive->file, saved_pos, SEEK_SET);
        }
    }

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
        assert(key.pos == key_prev.pos + val_prev.size);
    }
}

static void validate_tree(btree *index, compio_file *file, bool allow_empty = false) {
#ifndef NDEBUG
    DEBUG_PRINT("[VALIDATE_TREE]: current btree state for file with hash=%" PRIu64 ":\n", file->hash);
    auto file_range = index->get_range(tree_key{file->hash, 0}, tree_key{file->hash, UINT64_MAX});
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

static uint64_t compio_write_impl(const void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_write_impl(cursor=%" PRIu64 ", size=%" PRIu64 ", file_size=%" PRIu64 ")\n", file->cursor, size, file->size);

    const auto archive = file->archive;
    const auto block_reader = archive->block_reader;
    const uint64_t block_size = archive->config.block_size;
    const uint64_t block_size__minimum = archive->config.block_size__minimum;
    const uint64_t block_size__maximum = archive->config.block_size__maximum;

    if (archive->mode_b & mode_bit::r) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

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

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    compio::TransactionGuard txn(wal_ptr);

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
    if (write_start < last_block_end) {
        // get blocks range from b-tree
        const tree_key key_min = {file->hash, write_start};
        const tree_key key_max = {file->hash, write_end};
        auto range = archive->index->get_range(key_min, key_max);
        assert(!range.empty());
        assert(range.front().first.pos <= write_start);
        assert(range.back().first.pos + range.back().second.size >= write_start);
        validate_no_gaps_in_range(range);

        // enable temporary index, so it will fix expired tree_vals, that we will have in our range
        block_reader->enable_temporary_index();
        for (const auto &[key, val] : range) {
            // read and decompress block from file
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

            // actual block range within file (block-range)
            const uint64_t block_start = key.pos;
            const uint64_t block_end = key.pos + b->size();
            
            // block-range and write-range intersection
            const uint64_t copy_end = std::min(write_end, block_end);
            const uint64_t copy_start = std::max(write_start, block_start);
            assert(copy_end > copy_start); // if not, btree::get_range is broken

            // number of actual bytes from ptr, that we need to copy
            const uint64_t copy_size = copy_end - copy_start;
            // write offset within block-range
            const uint64_t dec_offset =
                (write_start > block_start) ? (write_start - block_start) : 0;
            DEBUG_PRINT("[CW] EXISTING BLOCK DATA: (%" PRIu64 ", %" PRIu64 ", %" PRIu64 ")\n", dec_offset, copy_size, b->size() - dec_offset - copy_size);

            std::copy_n(p_ptr, copy_size, b->data() + dec_offset);
            p_ptr += copy_size;
            ptr_bytes_written += copy_size;
            file->cursor += copy_size;
            file_table_item->size = file->size = std::max(file->cursor, file->size);
        }
        block_reader->disable_temporary_index();
    }

    // TODO: write new data into last block if size is small

    // if we still have bytes in ptr, we need to create new blocks
    if (ptr_bytes_written < size) {
        // append blocks to the end of the file

        // last block already has zero padding from the right, 
        // so we shift file->size directly to it's end
        file_table_item->size = file->size = last_block_end; 

        // total number of zeros we need to fill in
        uint64_t n_zeros = (write_start > last_block_end) ? (write_start - last_block_end) : 0;
        // total number of bytes we need to append to file
        uint64_t total_bytes_left = n_zeros + (size - ptr_bytes_written);
        uint64_t new_block_start = last_block_end;

        while (total_bytes_left > 0) {
            uint64_t current_block_size;
#ifdef COMPIO_DISABLE_INSERT_ERASE
            current_block_size = block_size;
#else
            if (total_bytes_left < block_size ||
                total_bytes_left - block_size < block_size__minimum) {
                current_block_size = total_bytes_left;
            } else {
                current_block_size = block_size;
            }
            assert(current_block_size <= block_size__maximum);
            assert(current_block_size <= total_bytes_left);
#endif

            // size of zero-padding from the left
            const uint64_t left_pad = std::min(n_zeros, current_block_size);
            // number of actual bytes from ptr, that we need to copy
            const uint64_t copy_size = std::min(current_block_size - left_pad, size - ptr_bytes_written);
            // size of zero-padding from the right
            const uint64_t right_pad = current_block_size - left_pad - copy_size;
            DEBUG_PRINT("[CW] NEW BLOCK DATA: (%" PRIu64 ", %" PRIu64 ", %" PRIu64 ")\n", left_pad, copy_size, right_pad);

            const tree_key key{file->hash, new_block_start};
            DEBUG_PRINT("[CW]creating block ({%" PRIu64 ",%" PRIu64 "}-{?,%" PRIu64 "})\n", key.hash, key.pos,
                        current_block_size);
            const auto b = block_reader->create_block(current_block_size, key);

            std::fill_n(b->data(), left_pad, 0);
            std::copy_n(p_ptr, copy_size, b->data() + left_pad);
            std::fill_n(b->data() + left_pad + copy_size, right_pad, 0);

            p_ptr += copy_size;
            ptr_bytes_written += copy_size;
            new_block_start += current_block_size;
            file->cursor += copy_size;

            n_zeros -= left_pad;
            total_bytes_left -= left_pad + copy_size;
            file_table_item->size = file->size += left_pad + copy_size;
        }
    }

    validate_tree(archive->index, file);

    if (!txn.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return ptr_bytes_written;
    }

    assert(ptr_bytes_written == size);
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
        // should not happen, because compio_open_file creates ftable record
        WARNING_PRINT("warning: no such file in header.ftable\n");
        errno = ENOENT;
        return 0;
    }

    size = std::max(UINT64_C(0), std::min(size, file->size - file->cursor));
    if (size == 0) {
        return 0;
    }

    const uint64_t read_start = file->cursor;
    const uint64_t read_end = read_start + size;

    const tree_key key_min = {file->hash, read_start};
    const tree_key key_max = {file->hash, read_end};
    auto range = archive->index->get_range(key_min, key_max);
    assert(!range.empty());

    for (std::size_t i = 1; i < range.size(); ++i) {
        assert(range[i - 1].first.pos + range[i - 1].second.size == range[i].first.pos);
    }
    assert(range.front().first.pos <= read_start);
    assert(range.back().first.pos + range.back().second.size >= read_end);

    auto p_ptr = reinterpret_cast<uint8_t *>(ptr);
    uint64_t ptr_bytes_read = 0;

    // enable temporary index, so it will fix expired tree_vals, that we will have in our range
    block_reader->enable_temporary_index();
    for (const auto &[key, val] : range) {
        DEBUG_PRINT("[CR]reading block ({%" PRIu64 ",%" PRIu64 "}-{%" PRIu64 ",%" PRIu64 "})\n", key.hash, key.pos, val.addr,
                    val.size);
        const std::shared_ptr<const block> b = block_reader->read_block(val.addr, key);
        if (!b) {
            // failed to decompress
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            block_reader->disable_temporary_index();
            return ptr_bytes_read;
        }
        assert(b->size() == val.size);

        const uint64_t block_start = key.pos;
        const uint64_t block_end = key.pos + b->size();
        const uint64_t copy_end = std::min(std::min(read_end, block_end), file->size);
        const uint64_t copy_start = std::max(read_start, block_start);
        assert(copy_end > copy_start); // if not, btree::get_range is broken
        const uint64_t copy_size = copy_end - copy_start;
        const uint64_t dec_offset = (read_start > block_start) ? (read_start - block_start) : 0;
        DEBUG_PRINT("[CR]copying data of size %" PRIu64 " from block (offset=%" PRIu64 ")\n", copy_size,
                    dec_offset);
        assert(dec_offset + copy_size <= b->size());

        std::copy_n(b->data() + dec_offset, copy_size, p_ptr);
        p_ptr += copy_size;
        ptr_bytes_read += copy_size;
        file->cursor += copy_size;
    }
    block_reader->disable_temporary_index();

    validate_tree(archive->index, file);

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

    if (archive->mode_b & mode_bit::r) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

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

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    compio::TransactionGuard txn(wal_ptr);

    const tree_key cursor_key = {file->hash, file->cursor};
    const auto key_val = archive->index->get_block(cursor_key);

    // TODO: merge with existing block if size is small

    if (key_val.has_value()) {
        // split block into two
        const auto &[left_key, left_val] = key_val.value();
        const auto left_b = block_reader->read_block(left_val.addr, left_key);
        if (!left_b) {
            // failed to decompress
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            return 0;
        }
        assert(left_b->size() == left_val.size);

        assert(left_key.pos < file->cursor);
        assert(left_key.pos + left_b->size() > file->cursor);
        const uint64_t left_size = file->cursor - left_key.pos;
        const uint64_t right_size = left_b->size() - left_size;
        left_b->shrink(left_size);
        const auto right_b = block_reader->create_block(right_size, cursor_key);
        std::copy_n(left_b->data() + left_size, right_size, right_b->data());
    }

    // shift blocks after cursor
    const tree_key file_end_key{file->hash, UINT64_MAX};
    archive->index->add_to_range(size, cursor_key, file_end_key);
    block_reader->add_to_range(size, cursor_key, file_end_key);

    auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);
    uint64_t total_bytes_left = size;

    // write new data from p_ptr into new blocks
    const uint64_t block_size = archive->config.block_size;
    const uint64_t block_size__minimum = archive->config.block_size__minimum;
    const uint64_t block_size__maximum = archive->config.block_size__maximum;
    while (total_bytes_left > 0) {
        uint64_t current_block_size;
        if (total_bytes_left < block_size || total_bytes_left - block_size < block_size__minimum) {
            current_block_size = total_bytes_left;
        } else {
            current_block_size = block_size;
        }
        assert(current_block_size <= block_size__maximum);
        assert(current_block_size <= total_bytes_left);

        const tree_key key{file->hash, file->cursor};
        const auto b = block_reader->create_block(current_block_size, key);
        std::copy_n(p_ptr, current_block_size, b->data());

        p_ptr += current_block_size;
        file->cursor += current_block_size;
        file->size += current_block_size;
        file_table_item->size += current_block_size;
        total_bytes_left -= current_block_size;
    }

    validate_tree(archive->index, file);

    if (!txn.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return 0;
    }

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

    if (archive->mode_b & mode_bit::r) {
        WARNING_PRINT("warning: can't compio_write to read-only file\n");
        errno = EROFS;
        return 0;
    }

    // Start transaction
    bool can_write = (archive->mode_b & mode_bit::w) || 
                     (archive->mode_b & mode_bit::a) || 
                     (archive->mode_b & mode_bit::plus);
    
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    compio::TransactionGuard txn(wal_ptr);

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
    auto range = archive->index->get_range(key_min, key_max);
    assert(!range.empty());
    assert(range.front().first.pos <= erase_start);
    assert(range.back().first.pos + range.back().second.size >= erase_end);
    validate_no_gaps_in_range(range);

    uint64_t bytes_erased = 0;

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
                DEBUG_PRINT("[CE]---moving by offset=%" PRIu64 "\n", block_erase_size);
                archive->index->add_to_range(block_erase_size, key, key);
                if (!block_reader->cache_contains(key)) {
                    // if out block not in cache (if cache_size=0), then block_reader->add_to_range
                    // won't update it's key, and invalid key will be written into file, so we
                    // manually shift key for this block
                    b->shift_key(block_erase_size);
                }
                block_reader->add_to_range(block_erase_size, key, key);
            }
        } else {
            // remove block completely
            DEBUG_PRINT("[CE]---removing block\n");
            block_reader->remove_block(b);
        }

        bytes_erased += block_erase_size;
        file->size -= block_erase_size;
    }

    // shift blocks after cursor to the left
    const tree_key file_end_key{file->hash, UINT64_MAX};
    const int64_t shift = -static_cast<int64_t>(bytes_erased);
    archive->index->add_to_range(shift, key_max, file_end_key);
    block_reader->add_to_range(shift, key_max, file_end_key);

    validate_tree(archive->index, file, true);

    if (!txn.commit(archive->file, archive->config.wal_max_size_bytes)) {
        errno = EIO;
        return 0;
    }

    return bytes_erased;
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

    // Start atomic transaction for the entire flush operation using RAII guard
    // Pass nullptr if we shouldn't use WAL, so guard becomes no-op
    compio::WalManager* wal_ptr = (archive->wal && can_write) ? archive->wal.get() : nullptr;
    compio::TransactionGuard txn(wal_ptr);
    bool wal_active = (wal_ptr != nullptr);

    // Track whether all durability operations succeed; used to decide if we can safely checkpoint.
    bool durable = true;

    if (archive->block_reader) archive->block_reader->clear_cache();
    if (archive->block_reader) archive->block_reader->invalidate_temporary_index();
    if (archive->index) archive->index->clear_cache();

    // Save allocator state (updates header fields)
    if (archive->allocator && can_write) {
         archive->allocator->save_state(archive);
    }
    
    // Double-buffered Header Write
    flush_header_double_buffered(archive);
    
    // Commit transaction (this performs a single fsync on the WAL)
    if (wal_active) {
        if (!txn.commit(archive->file, archive->config.wal_max_size_bytes)) {
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
