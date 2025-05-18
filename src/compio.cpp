#define _CRT_SECURE_NO_WARNINGS

#include "compio.h"
#include "compio_file.hpp"
#include "file.hpp"
#include "utils.hpp"
#include "allocator.hpp"
#include "debug_print.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace compio;

void compio_build_default_config(compio_config* result) {
    result->b_tree_degree = 16;
    compio_build_zlib_compressor(&result->compressor);
    result->fill_holes_with_zeros = true;
    result->block_size = 4096;
    result->cache_size = 128;
    result->block_cache_size = 16;
    result->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    result->fragmentation_threshold = 30;
}

compio_archive::compio_archive(FILE* file, uint8_t mode_b, const compio_config* config)
    : file(file),
      config(config),
      mode_b(mode_b), 
      block_reader(file, config->block_cache_size) {
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

    // Load allocator state if it exists
    if (archive->header->allocator_state_offset != 0 &&
        archive->header->allocator_state_size > 0) {
        archive->allocator->blocks_manager_.load_from_file(archive);
    }

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
    // TODO: remove all blocks from this file
    return archive->header->ftable.remove(name);
}

int compio_close_file(compio_file* file) {
    delete file;
    return 0;
}

int compio_close_archive(compio_archive* archive) {
    // 1) flush index nodes to file
    delete archive->index;

    // 2) clear block_reader cache to flush all blocks
    archive->block_reader.clear_cache();

    // 3) save blocks manager to the end of the file
    if (archive && archive->file && archive->allocator && archive->header) {
        archive->allocator->blocks_manager_.save_to_file(archive);
    }
    delete archive->allocator;
    
    // 4) flush header (important: do this after flushing allocator, because it saves offset and size in header)
    //
    // not calling `delete header`, because it's not a pointer created with new,
    // but a smart_infile_object, which will destroy and flush it's internal pointer 
    // (header in this case) when there'll be no smart_infile_objects pointing to this header
    //
    // thus, calling operator=({}) will destroy and flush our header
    archive->header = {};
    
    // 5) and finally we close the file
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
    const auto range = get_range_in_file(file, size);

    DEBUG_PRINT("[CW-1] range: {\n");
    for (const auto& elem : range) {
        DEBUG_PRINT("\t{%llu} -> (%llu)-(?)\n", elem.first.pos, elem.second.addr);
    }
    DEBUG_PRINT("}\n\n");

    // find file in file table (it must exist, because compio_file was
    // created with compio_open_file, which adds file into file table)
    auto file_table_item = file->archive->header->ftable.find(file->name);
    uint64_t fsize = file_table_item->size;

    auto config = file->archive->config;

    // start of segment to unpack into buffer
    uint64_t outer_segment_start = (range.size() > 0) ? range[0].first.pos : fsize;
    // end of segment to unpack into buffer
    uint64_t outer_segment_end = file->cursor + size;
    if (range.size() > 0) {
        const auto& last_block = range[range.size() - 1];
        outer_segment_end = std::max(outer_segment_end, last_block.first.pos + last_block.second.size);
    }
    // size of segment to unpack = size of buffer to allocate
    uint64_t outer_segment_size = outer_segment_end - outer_segment_start;
    if (outer_segment_end <= outer_segment_start) {
        return 0;
    }

    // offset inside of outer segment to write ptr data to
    int inner_offset = file->cursor - outer_segment_start;

    // create buffer for uncompressed data
    // if cursor is set after end of file, (eof, cursor) must be
    // filled with zeros, so we initialize buffer with zeros
    std::vector<uint8_t> buf(outer_segment_size, 0);

    // number of blocks to divide segment to after
    uint64_t n_blocks = (outer_segment_size + config->block_size - 1) / config->block_size;

    std::vector<uint8_t>::iterator p_buf = buf.begin();
    for (const auto& [key, val] : range) {
        auto block = file->archive->block_reader.read_block(val.addr);

        // decompress data from block->data into big buf
        uint64_t dst_size = block->original_size;
        if (block->is_compressed) {
            config->compressor.decompress(&(*p_buf), &dst_size, block->data.data(), block->data.size());
        } else {
            std::copy_n(block->data.begin(), block->data.size(), p_buf);
        }
        p_buf += dst_size;

        DEBUG_PRINT("[CW-2] deallocate {%llu} -> (%llu)-(%llu)\n", key.pos, val.addr, val.addr + STORAGE_BLOCK_METASIZE + block->size);

        // remove this block from file (we will add modified block as a new one)
        file->archive->allocator->deallocate(val.addr, STORAGE_BLOCK_METASIZE + block->size);

        // set removed=true, so it won't flush into file in destructor (smart_infile_object)
        block.remove();

        // also remove block from cache
        file->archive->block_reader.remove_block(block);
    }

    // modify uncompressed data in buffer with data from user (starting from inner_offset)
    std::copy_n(reinterpret_cast<const uint8_t*>(ptr), size, buf.begin() + inner_offset);

    // true, if block was updated in b-tree, otherwise false
    std::vector<bool> processed(range.size(), false);

    for (int i = 0; i < n_blocks; ++i) {
        // splitting uncompressed data into blocks of fixed size
        uint64_t offset = config->block_size * i;
        uint64_t b_start = outer_segment_start + offset;
        uint64_t uncompressed_size = std::min(outer_segment_end - b_start, (uint64_t)config->block_size);
        p_buf = buf.begin() + offset;

        uint64_t size = config->compressor.get_bufsize(uncompressed_size);
        std::vector<uint8_t> block_data(size);
        bool is_compressed = true;

        auto index_key = get_key(file->name, b_start);
        int ret = config->compressor.compress(block_data.data(), &size, &(*p_buf), uncompressed_size);
        if (ret != 0 || size > uncompressed_size) {
            // if failed to compress or compressed size > uncompressed size, write uncompressed data instead
            is_compressed = false;
            size = uncompressed_size;
            std::copy(p_buf, p_buf + uncompressed_size, block_data.begin());
        }

        // remove unneccesary bytes from buffer
        block_data.resize(size);

        // allocate memory in file for new block
        uint64_t addr = file->archive->allocator->allocate(STORAGE_BLOCK_METASIZE + size);

        auto block = file->archive->block_reader.create_block(addr, std::move(block_data));
        block->original_size = uncompressed_size;
        block->is_compressed = is_compressed;
        block->index_key = index_key;

        tree_val new_value = {addr, uncompressed_size};
        // if block already in b-tree, just update it, otherwise insert
        auto j = std::find_if(range.begin(), range.end(), [&block](const std::pair<tree_key, tree_val>& x) { return x.first == block->index_key; });
        if (j != range.end()) {
            DEBUG_PRINT("[CW-3] update {%llu} -> (%llu)-(%llu)\n", index_key.pos, new_value.addr, new_value.addr + STORAGE_BLOCK_METASIZE + block->size);
            processed[std::distance(range.begin(), j)] = true;
            file->archive->index->update(block->index_key, new_value);
        } else {
            DEBUG_PRINT("[CW-3] insert {%llu} -> (%llu)-(%llu)\n", index_key.pos, new_value.addr, new_value.addr + STORAGE_BLOCK_METASIZE + block->size);
            file->archive->index->insert(block->index_key, new_value);
        }
    }

    // remove blocks, that was not updated
    for (int i = 0; i < range.size(); ++i) {
        if (!processed[i]) {
            DEBUG_PRINT("[CW-3] remove {%llu} -> (%llu)-(?)\n", range[i].first.pos, range[i].second.addr);
            file->archive->index->remove(range[i].first);
        }
    }

    file_table_item->size = std::max(file_table_item->size, outer_segment_end);
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
        // index of last byte we need to read, in uncompressed block
        uint64_t end = std::min(current_offset + remaining_size, (int64_t)(val.size));
        // number of bytes copied into ptr on this iteration
        uint64_t bytes_copied = 0;
        if (end > current_offset) {
            // read block from file
            const auto block = file->archive->block_reader.read_block(val.addr);

            tmp_buf.resize(block->original_size);

            // decompress data from block data into tmp_buf
            uint64_t dst_size = block->original_size;
            if (block->is_compressed) {
                int ret = config->compressor.decompress(tmp_buf.data(), &dst_size, block->data.data(),
                                                        block->data.size());
                if (ret != 0) {
                    // invalid archive (wrong original size in storage block)
                    return static_cast<uint64_t>(static_cast<int64_t>(size) - remaining_size);
                }
            } else {
                std::copy_n(block->data.begin(), block->data.size(), tmp_buf.data());
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