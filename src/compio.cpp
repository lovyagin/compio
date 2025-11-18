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
    if (c->cache_size__nodes < 0) {
        WARNING_PRINT("warning: cache_size__nodes=%d < 0\n", c->cache_size__nodes);
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
        tree_key key_min = {hash_tail, 0};
        tree_key key_max = {hash_tail, UINT64_MAX}; // Get all blocks for this file
        auto all_blocks = archive->index->get_range(key_min, key_max);

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
    DEBUG_PRINT("\ncompio_seek(new_cursor=%ld)\n", new_cursor);
    return 0;
}

uint64_t compio_tell(compio_file *file) { return file->cursor; }

uint64_t compio_get_size(compio_file *file) { return file->size; }

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

    auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);
    uint64_t ptr_bytes_written = 0;

    if (write_start < file->size) {
        const tree_key key_min = {file->hash_tail, write_start};
        const tree_key key_max = {file->hash_tail, write_end};
        auto range = archive->index->get_range(key_min, key_max);
        assert(!range.empty());

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

        // enable temporary index, so it will fix expired tree_vals, that we will have in our range
        block_reader->enable_temporary_index();
        for (std::size_t range_idx = 0; range_idx < range.size(); ++range_idx) {
            // update existing blocks with new data
            const auto &[key, val] = range[range_idx];
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

            const uint64_t block_start = key.pos;
            const uint64_t block_end = key.pos + b->size();
            const uint64_t copy_end = std::min(write_end, block_end);
            const uint64_t copy_start = std::max(write_start, block_start);
            assert(copy_end > copy_start); // if not, btree::get_range is broken
            const uint64_t copy_size = copy_end - copy_start;
            const uint64_t dec_offset =
                (write_start > block_start) ? (write_start - block_start) : 0;
            DEBUG_PRINT("[CW]copying data of size %ld to block (offset=%ld)\n", copy_size,
                        dec_offset);

            std::copy_n(p_ptr, copy_size, b->data() + dec_offset);
            p_ptr += copy_size;
            ptr_bytes_written += copy_size;
            file->cursor += copy_size;
            assert(file->cursor <= file->size);
        }
        block_reader->disable_temporary_index();
    }

    if (ptr_bytes_written < size) {
        // append blocks to the end of the file
        uint64_t n_zeros = (file->cursor > file->size) ? (file->cursor - file->size) : 0;
        const uint64_t ptr_bytes_left = size - ptr_bytes_written;
        uint64_t total_bytes_left = n_zeros + ptr_bytes_left;
        uint64_t cursor = file->size;

        const uint64_t block_size = archive->config->block_size;
        const uint64_t block_size__minimum = archive->config->block_size__minimum;
        const uint64_t block_size__maximum = archive->config->block_size__maximum;
        while (total_bytes_left > 0) {
            uint64_t current_block_size;
            if (total_bytes_left < block_size ||
                total_bytes_left - block_size < block_size__minimum) {
                current_block_size = total_bytes_left;
            } else {
                current_block_size = block_size;
            }
            assert(current_block_size <= block_size__maximum);
            assert(current_block_size <= total_bytes_left);

            const uint64_t dec_offset = std::min(n_zeros, current_block_size);
            const uint64_t copy_size = current_block_size - dec_offset;

            const tree_key key{file->hash_tail, cursor};
            DEBUG_PRINT("[CW]creating block ({%lu,%lu}-{?,%lu})\n", key.hash, key.pos,
                        current_block_size);
            const auto b = block_reader->create_block(current_block_size, key);

            DEBUG_PRINT("[CW]filling %ld bytes of new block with zeros\n", dec_offset);
            std::fill_n(b->data(), dec_offset, 0);
            DEBUG_PRINT("[CW]copying data of size %ld to new block (offset=%ld)\n", copy_size,
                        dec_offset);
            std::copy_n(p_ptr, copy_size, b->data() + dec_offset);
            p_ptr += copy_size;
            ptr_bytes_written += copy_size;
            cursor += current_block_size;
            file->cursor += copy_size;

            n_zeros -= dec_offset;
            total_bytes_left -= current_block_size;
            file->size += current_block_size;
            file_table_item->size += current_block_size;
        }
    }

    assert(ptr_bytes_written == size);
    return size;
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

    const tree_key key_min = {file->hash_tail, read_start};
    const tree_key key_max = {file->hash_tail, read_end};
    auto range = archive->index->get_range(key_min, key_max);
    assert(!range.empty());

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
    uint64_t ptr_bytes_read = 0;

    // enable temporary index, so it will fix expired tree_vals, that we will have in our range
    block_reader->enable_temporary_index();
    for (std::size_t range_idx = 0; range_idx < range.size(); ++range_idx) {
        const auto &[key, val] = range[range_idx];
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
        const uint64_t copy_end = std::min(read_end, block_end);
        const uint64_t copy_start = std::max(read_start, block_start);
        assert(copy_end > copy_start); // if not, btree::get_range is broken
        const uint64_t copy_size = copy_end - copy_start;
        const uint64_t dec_offset = (read_start > block_start) ? (read_start - block_start) : 0;
        DEBUG_PRINT("[CR]copying data of size %ld from block (offset=%ld)\n", copy_size,
                    dec_offset);

        std::copy_n(b->data() + dec_offset, copy_size, p_ptr);
        p_ptr += copy_size;
        ptr_bytes_read += copy_size;
        file->cursor += copy_size;
    }
    block_reader->disable_temporary_index();

    return ptr_bytes_read;
}

uint64_t compio_insert(const void *ptr, uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_insert(cursor=%lu, size=%lu)\n", file->cursor, size);

    if (file->cursor >= file->size) {
        return compio_write(ptr, size, file);
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

    const tree_key cursor_key = {file->hash_tail, file->cursor};
    const auto key_val = archive->index->get_block(cursor_key);

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

        assert(left_key.pos < file->cursor);
        assert(left_key.pos + left_b->size() > file->cursor);
        const uint64_t left_size = file->cursor - left_key.pos;
        const uint64_t right_size = left_b->size() - left_size;
        const auto right_b = block_reader->create_block(right_size, cursor_key);
        std::copy_n(left_b->data() + left_size, right_size, right_b->data());
        left_b->shrink(left_size);
    }

    // shift blocks after cursor
    const tree_key key_max{file->hash_tail, UINT64_MAX};
    archive->index->add_to_range(size, cursor_key, key_max);
    block_reader->add_to_range(size, cursor_key, key_max);

    auto p_ptr = reinterpret_cast<const uint8_t *>(ptr);
    uint64_t total_bytes_left = size;

    const uint64_t block_size = archive->config->block_size;
    const uint64_t block_size__minimum = archive->config->block_size__minimum;
    const uint64_t block_size__maximum = archive->config->block_size__maximum;
    while (total_bytes_left > 0) {
        uint64_t current_block_size;
        if (total_bytes_left < block_size || total_bytes_left - block_size < block_size__minimum) {
            current_block_size = total_bytes_left;
        } else {
            current_block_size = block_size;
        }
        assert(current_block_size <= block_size__maximum);
        assert(current_block_size <= total_bytes_left);

        const tree_key key{file->hash_tail, file->cursor};
        const auto b = block_reader->create_block(current_block_size, key);
        std::copy_n(p_ptr, current_block_size, b->data());

        p_ptr += current_block_size;
        file->cursor += current_block_size;
        file->size += current_block_size;
        file_table_item->size += current_block_size;
        total_bytes_left -= current_block_size;
    }

    return size;
}

uint64_t compio_erase(uint64_t size, compio_file *file) {
    DEBUG_PRINT("\ncompio_erase(cursor=%lu, size=%lu)\n", file->cursor, size);
    return 0;
}

void compio_flush(compio_archive *archive) {
    archive->block_reader->clear_cache();
    archive->index->clear_cache();
}
