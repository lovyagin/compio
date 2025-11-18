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
    result->fill_holes_with_zeros = false;
    result->block_size = 1 << 12;
    result->block_size__minimum = 1 << 9;
    result->block_size__maximum = 1 << 14;
    result->cache_size__nodes = 128;
    result->cache_size__blocks = 16;
    result->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    result->fragmentation_threshold = 30;
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
      config(config),
      index(nullptr),
      block_reader(nullptr),
      mode_b(mode_b),
      allocator(nullptr) {
    if (is_file_empty(file))
        header = smart_infile_object<compio::header>(file, 0, new compio::header());
    else
        header = smart_infile_object<compio::header>(file, 0);

    // btree constructor is called in compio_open_archive to break
    // the dependence cycle (archive -> index -> allocator -> archive)
    //
    // so if you use compio_archive constructor directly (without compio_open_archive),
    // you should call archive->index = new btree(archive) after this constructor call
    //
    // index = new btree(this);
}

bool compio_archive::is_readonly() const { return mode_b & mode_bit::r; }

compio_archive *compio_open_archive(const char *fp, const char *mode, const compio_config *c) {
    uint8_t mode_b = parse_mode(mode);
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
                               archive->allocator, file, c->cache_size__nodes);
    if (!archive->index) {
        WARNING_PRINT("warning: failed to allocate memory for btree\n");
        goto no_index;
    }

    archive->block_reader = new compio::storage_block_reader(
        file, archive->allocator, archive->index, &c->compressor, c->cache_size__blocks);
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

    file->hash_tail = fnv1a(name);

    return file;
}

int compio_remove_file(compio_archive *archive, const char *name) {
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
    const uint64_t hash_tail = fnv1a(name);

    if (file_size > 0) {
        std::vector<std::pair<tree_key, tree_val>> all_blocks;
        tree_key key_min = {hash_tail, 0};
        tree_key key_max = {hash_tail, UINT64_MAX}; // Get all blocks for this file
        archive->index->get_range(key_min, key_max, all_blocks);

        // Save current file position
        long saved_pos = ftell(archive->file);

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
            fseek(archive->file, saved_pos, SEEK_SET);
        }
    }

    // Remove file from file table
    return archive->header->ftable.remove(name);
}

int compio_close_file(compio_file *file) {
    if (!file) {
        WARNING_PRINT("warning: passed nullptr into compio_close_file\n");
        return -1;
    }

    delete file;
    return 0;
}

int compio_close_archive(compio_archive *archive) {
    if (!archive) {
        WARNING_PRINT("warning: passed nullptr into compio_close_archive\n");
        return -1;
    }

    // 1) flush cached data to file and delete block_reader
    compio_flush(archive);
    delete archive->block_reader;

    // 2) save allocator state to the end of the file, if not read-only mode
    if (!(archive->mode_b & mode_bit::r) && archive->allocator) {
        if (!archive->allocator->save_state(archive)) {
            WARNING_PRINT("warning: failed to save allocator state\n");
            return -3;
        }
    }

    // 3) delete allocator
    delete archive->allocator;

    // 4) delete btree (it actually depends on allocator, but allocator also depends on index,
    // however they don't call each other in their destructors, so their destruction order does not
    // matter)
    delete archive->index;

    // 5) flush header
    // not calling `delete header`, because it's not a pointer created with new,
    // but a smart_infile_object, which will destroy and flush it's internal pointer
    archive->header = {};

    // 6) finally we close the file (block_reader, allocator and index are all deleted, so no
    // fwrites will be called after this)
    if (fclose(archive->file)) {
        WARNING_PRINT("warning: failed to close file in compio_close_archive\n");
        return -2;
    }
    DEBUG_PRINT("[cca]: closed file\n");

    // 7) and delete archive structure
    delete archive;

    return COMPIO_SUCCESS;
}

int compio_seek(compio_file *file, int64_t offset, uint8_t origin) {
    int64_t new_cursor = file->cursor;
    switch (origin) {
    case COMP_SEEK_SET:
        new_cursor = offset;
        break;
    case COMP_SEEK_CUR:
        new_cursor += offset;
        break;
    case COMP_SEEK_END:
        new_cursor = file->size + offset;
        break;
    }

    if (new_cursor < 0) {
        errno = EINVAL;
        return -1;
    }

    file->cursor = new_cursor;
    return 0;
}

uint64_t compio_tell(compio_file *file) { return file->cursor; }

uint64_t compio_write(const void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_write(cursor=%lu, size=%lu)\n", file->cursor, size);

    const auto archive = file->archive;
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

    const uint64_t write_start = file->cursor;
    const uint64_t write_end = write_start + size;
    uint64_t written_bytes = 0;

    // TODO: get rid of get_range if file->cursor >= file->size
    const tree_key key_min = {file->hash_tail, write_start};
    const tree_key key_max = {file->hash_tail, write_end};
    std::vector<std::pair<tree_key, tree_val>> range;
    archive->index->get_range(key_min, key_max, range);

    DEBUG_PRINT("[CW]b-tree range:\n");
    for (const auto &[key, val] : range) {
        DEBUG_PRINT("\t(key.pos=%lu) --- (val.addr=%lu, val.size=%lu)\n", key.pos, val.addr,
                    val.size);
    }
    for (std::size_t i = 1; i < range.size(); ++i) {
        assert(range[i - 1].first.pos + range[i - 1].second.size == range[i].first.pos);
    }
    if (!range.empty()) {
        assert(range.front().first.pos <= write_start);
        assert(range.back().first.pos + range.back().second.size >= write_start);
    }

    auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);

    for (std::size_t range_idx = 0; range_idx < range.size(); ++range_idx) {
        // update existing blocks with new data
        const auto &[key, val] = range[range_idx];
        const uint64_t block_start = key.pos;
        const uint64_t block_end = key.pos + val.size;
        DEBUG_PRINT("[CW]reading block ({%lu,%lu}-{%lu,%lu})\n", key.hash, key.pos, val.addr,
                    val.size);
        const auto b = block_reader->read_block(val.addr, key);
        if (!b) {
            // failed to decompress
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            goto end;
        }
        assert(b->size() == val.size);

        const uint64_t copy_end = std::min(write_end, block_end);
        const uint64_t copy_start = std::max(write_start, block_start);
        assert(copy_end > copy_start); // if not, btree::get_range is broken
        const uint64_t copy_size = copy_end - copy_start;
        const uint64_t dec_offset = (write_start > block_start) ? (write_start - block_start) : 0;
        DEBUG_PRINT("[CW]copying data of size %ld to block (offset=%ld)\n", copy_size, dec_offset);

        std::copy_n(p_ptr, copy_size, b->data() + dec_offset);
        written_bytes += copy_size;
        p_ptr += copy_size;
    }

    if (written_bytes < size) {
        // append block to the end of the file
        const uint64_t n_zeros = (file->cursor + written_bytes > file->size)
                                     ? (file->cursor + written_bytes - file->size)
                                     : 0;
        const uint64_t copy_size = size - written_bytes;
        const uint64_t bsize = n_zeros + copy_size;
        const tree_key key{file->hash_tail, file->size};
        DEBUG_PRINT("[CW]creating block ({%lu,%lu}-{?,%lu})\n", key.hash, key.pos, bsize);
        assert(bsize == write_end - file->size);
        const auto b = block_reader->create_block(bsize, key);

        DEBUG_PRINT("[CW]filling %ld bytes of new block with zeros\n", n_zeros);
        std::fill_n(b->data(), n_zeros, 0);
        DEBUG_PRINT("[CW]copying data of size %ld to new block (offset=%ld)\n", copy_size, n_zeros);
        std::copy_n(p_ptr, copy_size, b->data() + n_zeros);
        written_bytes += copy_size;
    }

end:
    block_reader->clear_temporary_index();
    file->size = file_table_item->size = std::max(file->cursor + written_bytes, file->size);
    file->cursor += written_bytes;
    return written_bytes;
}

uint64_t compio_read(void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_read(cursor=%lu, size=%lu)\n", file->cursor, size);

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
    uint64_t read_bytes = 0;

    const tree_key key_min = {file->hash_tail, read_start};
    const tree_key key_max = {file->hash_tail, read_end};
    std::vector<std::pair<tree_key, tree_val>> range;
    archive->index->get_range(key_min, key_max, range);

    DEBUG_PRINT("[CR]b-tree range:\n");
    for (const auto &[key, val] : range) {
        DEBUG_PRINT("\t(key.pos=%lu) --- (val.addr=%lu, val.size=%lu)\n", key.pos, val.addr,
                    val.size);
    }

    for (std::size_t i = 1; i < range.size(); ++i) {
        assert(range[i - 1].first.pos + range[i - 1].second.size == range[i].first.pos);
    }
    assert(range.front().first.pos <= read_start);
    assert(range.back().first.pos + range.back().second.size >= read_end);

    auto p_ptr = reinterpret_cast<uint8_t *>(ptr);

    for (std::size_t range_idx = 0; range_idx < range.size(); ++range_idx) {
        const auto &[key, val] = range[range_idx];
        const uint64_t block_start = key.pos;
        const uint64_t block_end = key.pos + val.size;
        DEBUG_PRINT("[CR]reading block ({%lu,%lu}-{%lu,%lu})\n", key.hash, key.pos, val.addr,
                    val.size);
        const std::shared_ptr<const block> b = block_reader->read_block(val.addr, key);
        if (!b) {
            // failed to decompress
            WARNING_PRINT("warning: failed to decompress data (compressed block is corrupted)\n");
            errno = EIO;
            goto end;
        }
        assert(b->size() == val.size);

        const uint64_t copy_end = std::min(read_end, block_end);
        const uint64_t copy_start = std::max(read_start, block_start);
        assert(copy_end > copy_start); // if not, btree::get_range is broken
        const uint64_t copy_size = copy_end - copy_start;
        const uint64_t dec_offset = (read_start > block_start) ? (read_start - block_start) : 0;
        DEBUG_PRINT("[CR]copying data of size %ld from block (offset=%ld)\n", copy_size,
                    dec_offset);

        p_ptr = std::copy_n(b->data() + dec_offset, copy_size, p_ptr);
        read_bytes += copy_size;
    }

end:
    file->cursor += read_bytes;
    return read_bytes;
}

void compio_flush(compio_archive *archive) {
    archive->block_reader->clear_cache();
    archive->index->clear_cache();
}
