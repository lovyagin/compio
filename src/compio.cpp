#include "compio.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>

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
    h.read_from(file, 0);
    *t = (compio_compression_type)h.compression_type;

    fclose(file);
    return 0;
}

compio_archive::compio_archive(FILE *file, uint8_t mode_b, const compio_config *config)
    : file(file),
      config(*config),
      index(nullptr),
      block_reader(nullptr),
      allocator(nullptr),
      mode_b(mode_b),
      open_files_count(0) {
    if (is_file_empty(file))
        header = smart_infile_object<compio::header>(file, 0,
                     new compio::header(static_cast<uint32_t>(config->max_files)), &io_mutex);
    else
        header = smart_infile_object<compio::header>(file, 0, &io_mutex);

    // btree constructor is called in compio_open_archive to break
    // the dependence cycle (archive -> index -> allocator -> archive)
    //
    // so if you use compio_archive constructor directly (without compio_open_archive),
    // you should call archive->index = new btree(archive) after this constructor call
    //
    // index = new btree(this);
}

bool compio_archive::is_readonly() const { return mode_b & mode_bit::r; }

static bool validate_config(const compio_config *c) {
    if (c->block_size <= 0) {
        WARNING_PRINT("warning: block_size=%d <= 0\n", c->block_size);
        return false;
    }
    if (c->block_size__minimum < 0) {
        WARNING_PRINT("warning: block_size__minimum=%d < 0\n", c->block_size__minimum);
        return false;
    }
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
    if (c->b_tree_degree <= 0) {
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
    if (c->max_files <= 0) {
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
    if (!validate_config(c)) {
        errno = EINVAL;
        goto end;
    }

    uint8_t mode_b;
    mode_b = parse_mode(mode);
    if (!mode_b) {
        errno = EINVAL;
        goto end;
    }

    // if w+ passed as mode, we have to clear file contents (using w+)
    // otherwise we open with a+ mode to read and write
    const char *archive_open_mode;
    if (mode_b & mode_bit::w) {
        archive_open_mode = "wb+";
    } else if (mode_b & mode_bit::a) {
        archive_open_mode = "ab+";
    } else {
        archive_open_mode = "rb";
    }

    FILE *file;
    file = fopen(fp, archive_open_mode);
    if (file == nullptr) {
        goto end;
    }

    compio_archive *archive;
    archive = new compio_archive(file, mode_b, c);
    if (!archive) {
        WARNING_PRINT("warning: failed to allocate memory for compio_archive\n");
        goto no_archive;
    }

    bool is_new_file;
    is_new_file = is_file_empty(file);
    if (!is_new_file) {
        // Validate that file is large enough to contain a complete header
        fseek64(file, 0, SEEK_END);
        int64_t actual_size = ftell64(file);
        if (actual_size < static_cast<int64_t>(sizeof(compio::header))) {
            WARNING_PRINT("warning: archive file truncated (size=%lld, need>=%lu)\n",
                          (long long)actual_size, (unsigned long)sizeof(compio::header));
            errno = EINVAL;
            goto no_allocator;
        }
    }
    if (is_new_file) {
        archive->header->compression_type = c->compressor.compression_type;
    } else if (readonly(archive->header, header)->compression_type !=
               c->compressor.compression_type) {
        // compression type mismatch
        errno = EINVAL;
        WARNING_PRINT("warning: compression type mismatch while opening archive\n");
        goto no_allocator;
    }

    // initialize allocator before btree, because btree uses allocator for creating root node
    archive->allocator = new compio::block_allocator(archive);
    if (!archive->allocator) {
        WARNING_PRINT("warning: failed to allocate memory for allocator\n");
        goto no_allocator;
    }

    archive->index = new btree(c->b_tree_degree, mode_b & mode_bit::r, archive->header,
                               archive->allocator, file, c->cache_size__nodes, &archive->io_mutex);
    if (!archive->index) {
        WARNING_PRINT("warning: failed to allocate memory for btree\n");
        goto no_index;
    }

    archive->block_reader = new compio::storage_block_reader(
        file, archive->allocator, archive->index,
        &archive->config.compressor, archive->config.cache_size__blocks, &archive->io_mutex);
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
end:
    return NULL;
}

compio_file *compio_open_file(const char *name, compio_archive *archive) {
    if (!archive) {
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
    strncpy(file->name, name, COMPIO_FNAME_MAX_SIZE);

    file->hash = fnv1a(name);

    archive->open_files_count++;

    return file;
}

int compio_remove_file(compio_archive *archive, const char *name) {
    if (!archive) {
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

    // 4) save allocator state to the end of the file, if not read-only mode
    if (!(archive->mode_b & mode_bit::r) && archive->allocator) {
        if (!archive->allocator->save_state(archive)) {
            WARNING_PRINT("warning: failed to save allocator state\n");
            return -3;
        }
    }

    // 5) delete allocator
    delete archive->allocator;

    // 6) delete btree (it actually depends on allocator, but allocator also depends on index,
    // however they don't call each other in their destructors, so their destruction order does not
    // matter)
    delete archive->index;

    // 7) flush header
    // not calling `delete header`, because it's not a pointer created with new,
    // but a smart_infile_object, which will destroy and flush it's internal pointer
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
    DEBUG_PRINT("\ncompio_seek(new_cursor=%ld)\n", new_cursor);
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
    DEBUG_PRINT("[VALIDATE_TREE]: current btree state for file with hash=%lu:\n", file->hash);
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
    DEBUG_PRINT("\ncompio_write_impl(cursor=%lu, size=%lu, file_size=%lu)\n", file->cursor, size, file->size);

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
            DEBUG_PRINT("[CW]reading block ({%lu,%lu}-{%lu,%lu})\n", key.hash, key.pos, val.addr,
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
            DEBUG_PRINT("[CW] EXISTING BLOCK DATA: (%lu, %lu, %lu)\n", dec_offset, copy_size, b->size() - dec_offset - copy_size);

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
            DEBUG_PRINT("[CW] NEW BLOCK DATA: (%lu, %lu, %lu)\n", left_pad, copy_size, right_pad);

            const tree_key key{file->hash, new_block_start};
            DEBUG_PRINT("[CW]creating block ({%lu,%lu}-{?,%lu})\n", key.hash, key.pos,
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
    DEBUG_PRINT("\ncompio_read(cursor=%lu, size=%lu, file_size=%lu)\n", file->cursor, size, file->size);

    if (!file || !file->archive) {
        return 0;
    }
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
        DEBUG_PRINT("[CR]reading block ({%lu,%lu}-{%lu,%lu})\n", key.hash, key.pos, val.addr,
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
        DEBUG_PRINT("[CR]copying data of size %ld from block (offset=%ld)\n", copy_size,
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
    DEBUG_PRINT("\ncompio_insert(cursor=%lu, size=%lu)\n", file->cursor, size);

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

    return size;
}

uint64_t compio_erase(uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_erase(cursor=%lu, size=%lu)\n", file->cursor, size);

    if (!file || !file->archive) {
        return 0;
    }
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
        DEBUG_PRINT("[CE]reading block ({%lu,%lu}-{%lu,%lu})\n", key.hash, key.pos, val.addr,
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

        DEBUG_PRINT("[CE]block=(%lu, %lu), erase=(%lu, %lu) -> block_erase=(%lu, %lu)\n",
                    block_start, block_end, erase_start, erase_end, block_erase_start,
                    block_erase_end);
        const uint64_t left_size = erase_start_offset;
        const uint64_t right_size = block_end - block_erase_end;
        DEBUG_PRINT("[CE]---left_size=%lu, erase_size=%lu, right_size=%lu\n", left_size,
                    block_erase_size, right_size);

        // TODO: merge with adjacent block if new size is small

        if (block_erase_size < b->size()) {
            // keep block in btree, but update key.pos and val.size
            if (right_size > 0) {
                DEBUG_PRINT("[CE]---copying %lu bytes from offset=%lu to offset=%lu\n", right_size,
                            erase_end_offset, erase_start_offset);
                std::copy(b->data() + erase_end_offset, b->data() + b->size(),
                          b->data() + erase_start_offset);
            }
            DEBUG_PRINT("[CE]---shrinking from size=%lu to size=%lu\n", b->size(),
                        b->size() - block_erase_size);
            b->shrink(b->size() - block_erase_size);
            if (left_size == 0) {
                DEBUG_PRINT("[CE]---moving by offset=%lu\n", block_erase_size);
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

    return bytes_erased;
}

void compio_flush(compio_archive *archive) {
    if (!archive) return;
    std::unique_lock<std::shared_mutex> lock(archive->mutex);
    if (archive->block_reader) archive->block_reader->clear_cache();
    if (archive->index) archive->index->clear_cache();
}
