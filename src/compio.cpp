#include "compio.h"
#include "compio_file.hpp"
#include "file.hpp"
#include "utils.hpp"

#include <cstdlib>
#include <cstring>

using namespace compio;

void compio_build_default_config(compio_config* result) {
    // TODO: change these random values
    result->b_tree_degree = 16;
    compio_build_dummy_compressor(&result->compressor);
    result->fill_holes_with_zeros = true;
    result->block_size = 4096;
}

compio_archive* compio_open_archive(const char* fp, const char* mode, const compio_config* c) {
    auto archive = (compio_archive*)malloc(sizeof(compio_archive));
    if (!archive)
        return NULL;

    archive->mode_b = parse_mode(mode);
    if (!archive->mode_b) {
        free(archive);
        return NULL;
    }

    // if user passed "w" as mode, we still should open file as readable
    const char* archive_open_mode;
    if (archive->mode_b & append_bit)
        archive_open_mode = "a+";
    else
        archive_open_mode = "w+";

    archive->file = fopen(fp, archive_open_mode);
    if (!archive->file) {
        free(archive);
        return NULL;
    }

    fseek(archive->file, 0, SEEK_END);
    long fsize = ftell(archive->file);
    if (fsize == 0) {
        archive->header = new header();
        flush_header(archive);
    } else
        archive->header = new header(archive->file);

    archive->config = c;

    return archive;
}

compio_file* compio_open_file(const char* name, compio_archive* archive) {
    long name_len = strlen(name);
    if (name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    auto file_table_item = archive->header->ftable.find(name);
    if (file_table_item == nullptr) {
        if (archive->mode_b & write_bit) {
            file_table_item = archive->header->ftable.add(name);
            if (file_table_item == NULL) {
                errno = ENFILE;
                return NULL;
            }
            flush_header(archive);
        } else {
            errno = EROFS;
            return NULL;
        }
    }

    auto file = (compio_file*)malloc(sizeof(compio_file));
    if (file == NULL)
        return NULL;

    file->size = file_table_item->size;

    if (archive->mode_b & 0b100)
        file->cursor = file->size;
    else
        file->cursor = 0;

    file->archive = archive;
    strncpy(file->name, name, COMPIO_FNAME_MAX_SIZE);

    return file;
}

int compio_remove_file(compio_archive* archive, const char* name) {
    long name_len = strlen(name);
    if (name_len > COMPIO_FNAME_MAX_SIZE) {
        errno = ENAMETOOLONG;
        return -2;
    }

    return archive->header->ftable.remove(name);
}

int compio_close_file(compio_file* file) {
    free(file);
    return 0;
}

int compio_close_archive(compio_archive* archive) {
    if (fclose(archive->file))
        return -1;
    delete archive->header;
    free(archive);
    return 0;
}

int compio_seek(compio_file* file, uint64_t offset, uint8_t origin) {
    uint64_t new_cursor = file->cursor;
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

uint64_t compio_write(const void* ptr, uint64_t size, compio_file* file) {
    // 0. if index_root == NULL, btree_create at first
    // 1. get blocks indices from file->cursor and btree get_range_and_pop operation
    //      compio_btree_get_range(FILE* file->archive->file, uint64_t file->archive->header->index_root, file->cursor, file->cursor + size)
    // 2. unpack them using file->archive->config->compressor.decompress
    // 3. modify with data from ptr
    // 4. split into blocks of size file->archive->config->preferred_block_size
    // 5. compress
    // 6. push new blocks into b-tree
    // 7. update 
    //      - file->cursor, 
    //      - file->size, 
    //      - file->archive->header->ftable, 
    //      - (potentially) file->archive->header->index_root
    // 8. flush header
}

uint64_t compio_read(void* ptr, uint64_t size, compio_file* file) {
    // 1. steps 0-2 from compio_write (but without pop)
    // 2. write size bytes into ptr
}
