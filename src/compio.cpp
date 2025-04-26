#define _CRT_SECURE_NO_WARNINGS

#include "compio.h"
#include "compio_file.hpp"
#include "file.hpp"
#include "utils.hpp"
#include "allocator.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace compio;

void compio_build_default_config(compio_config* result) {
    result->b_tree_degree = 16;
    compio_build_dummy_compressor(&result->compressor);
    result->fill_holes_with_zeros = true;
    result->swap_endianness = false;
    result->block_size = 4096;
    result->cache_size = -1;
    result->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    result->fragmentation_threshold = 30;
}

compio_archive::compio_archive(FILE* file, uint8_t mode_b, const compio_config* config)
    : file(file),
      config(config),
      mode_b(mode_b) {
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    if (fsize == 0)
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

compio_archive* compio_open_archive(const char* fp, const char* mode, const compio_config* c) {
    uint8_t mode_b = parse_mode(mode);
    if (!mode_b) {
        errno = EINVAL;
        return NULL;
    }

    // if w+ passed as mode, we have to clear file contents (using w+)
    //, otherwise we open with a+ mode to read and write
    const char* archive_open_mode;
    if (mode_b & mode_bit::w)
        archive_open_mode = "w+";
    else
        archive_open_mode = "a+";

    auto file = fopen(fp, archive_open_mode);
    if (file == nullptr)
        return NULL;

    auto archive = new compio_archive(file, mode_b, c);

    // initialize allocator before btree, because btree uses allocator for creating root node
    archive->allocator = new compio::block_allocator(archive);
    archive->index = new btree(archive);

    return archive;
}

compio_file* compio_open_file(const char* name, compio_archive* archive) {
    if (size_t name_len = strlen(name); name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    auto file_table_item = readonly(archive->header, compio::header)->ftable.find(name);
    if (file_table_item == nullptr) {
        // if mode != "r"
        if (!((archive->mode_b & mode_bit::r) && (!(archive->mode_b & mode_bit::plus)))) {
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

    return file;
}

int compio_remove_file(compio_archive* archive, const char* name) {
    if (size_t name_len = strlen(name); name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return -2;
    }

    return archive->header->ftable.remove(name);
}

int compio_close_file(compio_file* file) {
    delete file;
    return 0;
}

int compio_close_archive(compio_archive* archive) {
    // destroy and flush header before closing the file
    //
    // not calling `delete header`, because it's not a pointer created with new,
    // but a smart_infile_object, which will destroy and flush it's internal pointer 
    // (header in this case) when there'll be no smart_infile_objects pointing to this header
    //
    // thus, calling operator=({}) will destroy and flush our header
    archive->header = {};
    
    // do the same with btree node cache
    delete archive->index;

    // destroy allocator before closing file, so it can save it's state in it (todo)
    delete archive->allocator;

    // and finally we close the file
    if (fclose(archive->file))
        return -1;

    delete archive;
    return 0;
}

int compio_seek(compio_file* file, int64_t offset, uint8_t origin) {
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

uint64_t compio_tell(compio_file* file) { return file->cursor; }

static auto get_range_in_file(compio_file* file, uint64_t size) {
    // return range of blocks, that intersect [cursor, cursor + size)
    std::vector<std::pair<tree_key, tree_val>> range;
    auto key_min = get_key(file->name, file->cursor);
    auto key_max = get_key(file->name, file->cursor + size);
    file->archive->index->get_range(key_min, key_max, range);
    return range;
}

uint64_t compio_write(const void* ptr, uint64_t size, compio_file* file) {
    // get range of blocks, that intersect our workspace
    auto range = get_range_in_file(file, size);

    // find file in file table (it must exist, because compio_file was
    // created with compio_open_file, which adds file into file table)
    auto file_table_item = file->archive->header->ftable.find(file->name);
    uint64_t fsize = file_table_item->size;

    uint64_t start = (range.size() > 0) ? range[0].first.pos : fsize;
    uint64_t end = file->cursor + size;
    // create buffer for uncompressed data
    // if cursor is set after end of file, (eof, cursor) must be
    // filled with zeros, so we initialize buffer with zeros
    std::vector<uint8_t> buf(end - start, 0);

    auto config = file->archive->config;
    uint64_t n_blocks = (end - start + config->block_size - 1) / config->block_size;

    std::vector<uint8_t>::iterator p_buf = buf.begin();
    // temporary buffer for compressed data from one block
    std::vector<uint8_t> tmp_buf;
    tmp_buf.reserve(config->block_size);
    for (const auto& [key, val] : range) {
        smart_infile_object<storage_block> block(file->archive->file, val.addr);

        // copy compressed data from block into tmp_buf
        tmp_buf.resize(block->size);
        std::copy(block->data.begin(), block->data.end(), tmp_buf.begin());

        // decompress data from tmp_buf into big buf
        uint64_t dst_size = block->original_size;
        config->compressor.decompress(&(*p_buf), &dst_size, tmp_buf.data(), tmp_buf.size());
        p_buf += dst_size;

        // remove this block from file (we will add modified block as a new one)
        file->archive->allocator->deallocate(val.addr, STORAGE_BLOCK_METASIZE + block->size);

        // set removed=true, so it won't flush into file in destructor (smart_infile_object)
        block.remove();
    }

    // modify uncompressed data in buffer with data from user
    std::copy_n(reinterpret_cast<const uint8_t*>(ptr), size, buf.begin());

    for (int i = 0; i < n_blocks; ++i) {
        // splitting uncompressed data into blocks of fixed size
        uint64_t offset = config->block_size * i;
        uint64_t b_start = start + offset;
        uint64_t uncompressed_size = std::min(end - b_start, (uint64_t)config->block_size);
        p_buf = buf.begin() + offset;

        std::vector<uint8_t> block_data(uncompressed_size);
        uint64_t size;

        auto index_key = get_key(file->name, b_start);
        int ret = config->compressor.compress(block_data.data(), &size, &(*p_buf), uncompressed_size);
        if (ret != 0) {
            // if compressed size > uncompressed size, write uncompressed data instead
            size = uncompressed_size;
            std::copy(p_buf, p_buf + uncompressed_size, block_data.begin());
        } else {
            // remove unneccesary bytes from buffer
            block_data.resize(size);
        }

        // allocate memory in file for new block
        uint64_t addr = file->archive->allocator->allocate(STORAGE_BLOCK_METASIZE + size);

        smart_infile_object<storage_block> block(file->archive->file, addr,
                                                 new storage_block(std::move(block_data)));
        block->original_size = uncompressed_size;
        block->is_compressed = ret == 0;
        block->index_key = index_key;

        tree_val new_value = {addr, uncompressed_size};
        // if block already in tree, just update it, otherwise insert
        if (std::find_if(range.begin(), range.end(),
                         [&block](const std::pair<tree_key, tree_val>& x) {
                             return x.first == block->index_key;
                         }) != range.end())
            file->archive->index->update(block->index_key, new_value);
        else
            file->archive->index->insert(block->index_key, new_value);
    }

    file_table_item->size = std::max(file_table_item->size, end);
    file->size = file_table_item->size;
    file->cursor += size;
    return size;
}

uint64_t compio_read(void* ptr, uint64_t size, compio_file* file) {
    auto file_table_item = readonly(file->archive->header, header)->ftable.find(file->name);
    uint64_t fsize = file_table_item->size;

    // if cursor is after end of file, we can't read anything
    size = std::min(size, fsize - file->cursor);
    if (size == 0) {
        return 0;
    }

    auto range = get_range_in_file(file, size);
    if (range.empty()) {
        return 0;
    }

    const auto config = file->archive->config;
    auto* p_buf = static_cast<uint8_t*>(ptr);

    // number of bytes to skip in the beginning
    int64_t current_offset = static_cast<int64_t>(file->cursor) - static_cast<int64_t>(range[0].first.pos);
    // number of bytes we've left to read
    auto remaining_size = static_cast<int64_t>(size);

    // temporary buffer for decompressing
    std::vector<uint8_t> tmp_buf;
    tmp_buf.reserve(config->block_size);

    for (auto& [key, val] : range) {
        // read block from file
        const smart_infile_object<storage_block> block(file->archive->file, val.addr);

        // index of last byte we need to read, in uncompressed block
        uint64_t end = std::min(current_offset + remaining_size, (int64_t)(val.size));
        // number of bytes copied into ptr on this iteration
        uint64_t bytes_copied = 0;
        if (end > current_offset) {
            tmp_buf.resize(block->original_size);

            // decompress data from block data into tmp_buf
            uint64_t dst_size = block->original_size;
            int ret = config->compressor.decompress(tmp_buf.data(), &dst_size, block->data.data(),
                                                    block->data.size());
            if (ret != 0) {
                // invalid archive (wrong original size in storage block)
                return static_cast<uint64_t>(static_cast<int64_t>(size) - remaining_size);
            }

            const auto bytes_to_copy = static_cast<int64_t>(end - static_cast<uint64_t>(current_offset));
            p_buf = std::copy_n(tmp_buf.begin() + current_offset, bytes_to_copy, p_buf);
            remaining_size -= bytes_to_copy;
        }

        current_offset -= val.size;
        if (current_offset < 0)
            current_offset = 0;
        remaining_size -= bytes_copied;
    }

    uint64_t bytes_read = static_cast<int64_t>(size) - remaining_size;
    file->cursor += bytes_read;
    return bytes_read;
}