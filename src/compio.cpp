#include "compio.h"

#include <algorithm>
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
    result->block_size = 4096;
    result->cache_size__nodes = 128;
    result->cache_size__blocks = 16;
    result->cache_size__compression = 16;
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
      mode_b(mode_b),
      dec_cache(config->cache_size__compression),
      c_buffer(new uint8_t[config->compressor.get_bufsize(config->block_size)]),
      block_reader(file, config->cache_size__blocks) {
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

compio_archive *compio_open_archive(const char *fp, const char *mode, const compio_config *c) {
    uint8_t mode_b = parse_mode(mode);
    if (!mode_b) {
        errno = EINVAL;
        goto end;
    }

    // if w+ passed as mode, we have to clear file contents (using w+)
    // otherwise we open with a+ mode to read and write
    const char *archive_open_mode;
    if (mode_b & mode_bit::w)
        archive_open_mode = "w+";
    else
        archive_open_mode = "a+";

    FILE *file;
    file = fopen(fp, archive_open_mode);
    if (file == nullptr) {
        goto end;
    }

    compio_archive *archive;
    archive = new compio_archive(file, mode_b, c);
    if (!archive) {
        WARNING_PRINT("warning: failed to allocate memory for compio_archive\n");
        goto end1;
    }

    bool is_new_file;
    is_new_file = is_file_empty(file);
    if (is_new_file) {
        archive->header->compression_type = c->compressor.compression_type;
    } else if (archive->header->compression_type != c->compressor.compression_type) {
        // compression type mismatch
        errno = EINVAL;
        WARNING_PRINT("warning: compression type mismatch while opening archive\n");
        goto end2;
    }

    // initialize allocator before btree, because btree uses allocator for creating root node
    archive->allocator = new compio::block_allocator(archive);
    if (!archive->allocator) {
        WARNING_PRINT("warning: failed to allocate memory for allocator\n");
        goto end2;
    }

    archive->index = new btree(archive);
    if (!archive->index) {
        WARNING_PRINT("warning: failed to allocate memory for btree\n");
        goto end3;
    }

    if (!is_new_file && !archive->allocator->load_state(archive)) {
        WARNING_PRINT("warning: failed to load allocator state from archive\n");
        goto end4;
    }

    return archive;

end4:
    delete archive->index;
end3:
    delete archive->allocator;
end2:
    delete archive;
end1:
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

    file->hash_tail = get_hash_tail(name);

    return file;
}

int compio_remove_file(compio_archive *archive, const char *name) {
    size_t name_len = strlen(name);
    if (name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return -2;
    }
    // TODO: remove all blocks from this file
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
    
    // 1) flush cached data to file
    compio_flush(archive);

    // 2) save allocator state to the end of the file
    if (archive->allocator) {
        if (!archive->allocator->save_state(archive)) {
            WARNING_PRINT("warning: failed to save allocator state\n");
            return -3;
        }
    }

    // 3) delete allocator
    delete archive->allocator;

    // 4) flush header
    // not calling `delete header`, because it's not a pointer created with new,
    // but a smart_infile_object, which will destroy and flush it's internal pointer
    archive->header = {};

    // 5) finally we close the file
    if (fclose(archive->file)) {
        WARNING_PRINT("warning: failed to close file in compio_close_archive\n");
        return -2;
    }

    // 6) and delete remaining structures
    delete archive->index;
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
    DEBUG_PRINT("\ncompio_write(cursor=%d, size=%d)\n", file->cursor, size);

    if (size == 0) {
        return 0;
    }

    const auto archive = file->archive;
    const auto config = archive->config;

    auto file_table_item = archive->header->ftable.find(file->name);
    const uint64_t fsize = file_table_item->size;
    const uint64_t block_size = config->block_size;

    const uint64_t write_start = file->cursor;
    const uint64_t write_end = write_start + size;
    uint64_t written_bytes = 0;

    const uint64_t n_existing_blocks = (fsize + block_size - 1) / block_size;
    const uint64_t start_block_idx = std::min(file->cursor / block_size, n_existing_blocks);
    const uint64_t end_block_idx = (file->cursor + size - 1) / block_size;

    auto range = get_range_in_file(file, size);
    uint64_t range_idx = 0;

    DEBUG_PRINT("[CW]b-tree range:\n");
    for (const auto &[key, val] : range) {
        DEBUG_PRINT("\t(key.pos=%d) --- (val.addr=%d, val.size=%d)\n", key.pos, val.addr, val.size);
    }

    const uint8_t *p_ptr = reinterpret_cast<const uint8_t *>(ptr);

    for (uint64_t i = start_block_idx; i <= end_block_idx; ++i, ++range_idx) {
        const uint64_t block_start = i * block_size;
        const uint64_t block_end = block_start + block_size;

        std::shared_ptr<uint8_t[]> dec_buffer;

        // read block from file and decompress it into dec_buffer (or take decompressed data from
        // cache)
        if (i < n_existing_blocks) {
            if (range_idx >= range.size()) {
                // this should not happen
                WARNING_PRINT("range_idx = %d >= %d = range.size()\n", range_idx, range.size());
                goto end;
            }
            const auto &[key, val] = range[range_idx];
            if (key.pos != i * block_size) {
                // this should not happen
                WARNING_PRINT("key.pos = %d != %d = i * block_size\n", key.pos, i * block_size);
                for (const auto &[key, val] : range) {
                    WARNING_PRINT("%d, %d - %d, %d\n", key.hash, key.pos, val.addr, val.size);
                }
                WARNING_PRINT("cur=%d, size=%d\n", file->cursor, size);
                WARNING_PRINT("fsize=%d\n", fsize);
                goto end;
            }

            uint64_t c_size;
            DEBUG_PRINT("[CW]want block on val.addr=%d\n", val.addr);

            if (archive->dec_cache.exists(val.addr)) {
                auto cache_elem = archive->dec_cache.pop(val.addr);
                dec_buffer = cache_elem.first;
                c_size = cache_elem.second;
                DEBUG_PRINT("[CW]compression_cache hit for val.addr=%d\n", val.addr);
            } else {
                if (config->cache_size__compression > 0 && archive->dec_cache.is_full()) {
                    // can reuse already allocated buffer
                    dec_buffer = archive->dec_cache.pop_back().first;
                    DEBUG_PRINT("[CW]compression_cache fault, reusing cache tail buffer\n");
                } else {
                    dec_buffer = std::shared_ptr<uint8_t[]>(new uint8_t[block_size]);
                    DEBUG_PRINT("[CW]compression_cache fault, allocating buffer\n");
                }

                auto block = archive->block_reader.read_block(val.addr);

                uint64_t dst_size = block->original_size;
                if (block->is_compressed) {
                    int ret = config->compressor.decompress(dec_buffer.get(), &dst_size,
                                                            block->data.get(), block->size);
                    if (ret != 0) {
                        // invalid archive (compressed data is too big after decompression)
                        // TODO: set appropriate errno
                        WARNING_PRINT(
                            "compressed data is too big after decompression (%d is not enough)\n",
                            dst_size);
                        goto end;
                    }
                } else {
                    std::copy_n(block->data.get(), block->size, dec_buffer.get());
                }

                c_size = block->size;
            }

            DEBUG_PRINT("[CW]removing and deallocating block val.addr=%d\n", val.addr);
            archive->block_reader.remove_block(val.addr);
            archive->allocator->deallocate(val.addr, STORAGE_BLOCK_METASIZE + c_size);
        } else {
            DEBUG_PRINT("[CW]zero-initializing new block\n");
            if (config->cache_size__compression > 0 && archive->dec_cache.is_full()) {
                dec_buffer = archive->dec_cache.pop_back().first;
            } else {
                dec_buffer = std::shared_ptr<uint8_t[]>(new uint8_t[block_size]);
            }
            std::fill_n(dec_buffer.get(), block_size, 0);
        }

        // copy data from ptr into dec_buffer
        int64_t copy_size =
            std::min<int64_t>(write_end, block_end) - std::max<int64_t>(write_start, block_start);
        if (copy_size > 0) {
            uint64_t dec_offset =
                std::max<int64_t>(0, static_cast<int64_t>(file->cursor) - block_start);
            uint64_t ptr_offset =
                std::max<int64_t>(0, static_cast<int64_t>(block_start) - file->cursor);
            std::copy_n(p_ptr + ptr_offset, copy_size, dec_buffer.get() + dec_offset);
            written_bytes += copy_size;
            DEBUG_PRINT("[CW]copied ptr data to dec_buffer\n");
        }

        // compress dec_buffer into c_buffer
        bool is_compressed = true;
        uint64_t c_buffer_size = config->compressor.get_bufsize(block_size);
        auto c_buffer = std::make_unique<uint8_t[]>(c_buffer_size);
        int ret = config->compressor.compress(c_buffer.get(), &c_buffer_size, dec_buffer.get(),
                                              block_size);
        if (ret != 0 || c_buffer_size > block_size) {
            if (ret != 0) {
                WARNING_PRINT("compressor->compress returned %d\n", ret);
            }

            is_compressed = false;
            std::copy_n(dec_buffer.get(), block_size, c_buffer.get());
            c_buffer_size = block_size;
        }

        // create new block
        uint64_t addr = archive->allocator->allocate(STORAGE_BLOCK_METASIZE + c_buffer_size);
        DEBUG_PRINT("[CW]creating block on addr=%d\n", addr);
        tree_key new_key = {file->hash_tail, block_start};
        tree_val new_val = {addr, block_size};

        auto block = archive->block_reader.create_block(addr, std::move(c_buffer), c_buffer_size);
        block->original_size = block_size;
        block->is_compressed = is_compressed;
        block->index_key = new_key;

        // insert to the tree (or update)
        if (i < n_existing_blocks) {
            archive->index->update(new_key, new_val);
        } else {
            archive->index->insert(new_key, new_val);
        }

        // save uncompressed data to cache
        archive->dec_cache.put(addr, {dec_buffer, c_buffer_size});
    }

end:
    file->size = file_table_item->size = std::max<uint64_t>(fsize, write_end);
    file->cursor += written_bytes;
    return written_bytes;
}

uint64_t compio_read(void *ptr, uint64_t size, compio_file *file) {
    const auto archive = file->archive;
    const auto config = archive->config;

    auto file_table_item = archive->header->ftable.find(file->name);
    const uint64_t fsize = file_table_item->size;
    const uint64_t block_size = config->block_size;
    const uint64_t cursor = file->cursor;

    size = std::max<uint64_t>(0, std::min<int64_t>(size, fsize - cursor));
    if (size == 0) {
        return 0;
    }

    const uint64_t start_block_idx = cursor / block_size;
    const uint64_t end_block_idx = (cursor + size - 1) / block_size;

    const uint64_t read_start = cursor;
    const uint64_t read_end = read_start + size;
    uint64_t read_bytes = 0;

    auto range = get_range_in_file(file, size);
    uint64_t range_idx = 0;

    uint8_t *p_ptr = reinterpret_cast<uint8_t *>(ptr);

    for (uint64_t i = start_block_idx; i <= end_block_idx; ++i, ++range_idx) {
        const uint64_t block_start = i * block_size;
        const uint64_t block_end = block_start + block_size;

        if (range_idx >= range.size()) {
            WARNING_PRINT("went out of range in compio_read (%d >= %d)\n", range_idx, range.size());
            goto end;
        }

        const auto &[key, val] = range[range_idx];

        std::shared_ptr<uint8_t[]> dec_buffer;
        if (archive->dec_cache.exists(val.addr)) {
            dec_buffer = archive->dec_cache.get(val.addr).first;
        } else {
            if (config->cache_size__compression > 0 && archive->dec_cache.is_full()) {
                dec_buffer = archive->dec_cache.pop_back().first;
            } else {
                dec_buffer = std::shared_ptr<uint8_t[]>(new uint8_t[block_size]);
            }

            const auto block = archive->block_reader.read_block(val.addr);

            uint64_t dst_size = block->original_size;
            if (block->is_compressed) {
                int ret = config->compressor.decompress(dec_buffer.get(), &dst_size,
                                                        block->data.get(), block->size);
                if (ret != 0) {
                    // invalid archive (compressed data is too big after decompression)
                    // TODO: set appropriate errno
                    WARNING_PRINT(
                        "compressed data is too big after decompression (%d is not enough)\n",
                        dst_size);
                    goto end;
                }
                archive->dec_cache.put(val.addr, {dec_buffer, block->size});
            } else {
                std::copy_n(block->data.get(), block->size, dec_buffer.get());
            }
        }

        int64_t copy_size =
            std::min<int64_t>(read_end, block_end) - std::max<int64_t>(read_start, block_start);
        if (copy_size > 0) {
            uint64_t dec_offset = std::max<int64_t>(0, static_cast<int64_t>(cursor) - block_start);
            uint64_t ptr_offset = std::max<int64_t>(0, static_cast<int64_t>(block_start) - cursor);
            std::copy_n(dec_buffer.get() + dec_offset, copy_size, p_ptr + ptr_offset);
            read_bytes += copy_size;
        }
    }

end:
    file->cursor += read_bytes;
    return read_bytes;
}

void compio_flush(compio_archive *archive) {
    archive->block_reader.clear_cache();
    archive->index->reader.clear_cache();
}

namespace compio {

std::vector<std::pair<tree_key, tree_val>> get_range_in_file(compio_file *file,
                                                                    uint64_t size) {
    // return range of blocks, that intersect [cursor, cursor + size)
    std::vector<std::pair<tree_key, tree_val>> range;

    // reserve number of blocks, that should be in the tree
    int64_t n_blocks = std::min<int64_t>(file->cursor + size, file->size);
    n_blocks -= static_cast<int64_t>(file->cursor);
    n_blocks = std::max<int64_t>(0l, n_blocks);
    n_blocks /= file->archive->config->block_size;
    range.reserve(n_blocks);

    tree_key key_min = {file->hash_tail, file->cursor};
    tree_key key_max = {file->hash_tail, file->cursor + size};
    file->archive->index->get_range(key_min, key_max, range);
    return range;
}

} // namespace compio
