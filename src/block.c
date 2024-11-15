#include "block.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

compio_block* compio_create_block(size_t size, int is_compressed) {
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (!block) return NULL;

    block->offset = 0;
    block->size = size;
    block->is_compressed = is_compressed;
    block->data = malloc(size);
    if (!block->data) {
        free(block);
        return NULL;
    }
    block->position = 0;
    block->fragmented = 0;
    block->fragments = NULL;
    block->fragment_count = 0;

    return block;
}

compio_block* compio_block_init(size_t offset, size_t size, int is_compressed, void* data) {
    compio_block* block = compio_create_block(size, is_compressed);
    if (!block) return NULL;

    block->offset = offset;
    if (data) {
        memcpy(block->data, data, size);
    }
    return block;
}

compio_block* compio_find_block_by_offset(compio_block* blocks, size_t num_blocks, size_t offset) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (blocks[i].offset == offset) {
            return &blocks[i];
        }
    }
    return NULL;
}

compio_fragment* compio_split_block_into_fragments(compio_block* block, size_t fragment_size, size_t* num_fragments) {
    if (!block || fragment_size == 0 || num_fragments == NULL) {
        return NULL;
    }

    size_t count = (block->size + fragment_size - 1) / fragment_size;
    compio_fragment* fragments = (compio_fragment*)malloc(sizeof(compio_fragment) * count);
    if (!fragments) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        fragments[i].offset = i * fragment_size;
        fragments[i].size = (i == count - 1) ? (block->size - i * fragment_size) : fragment_size;
        fragments[i].is_active = 1;
    }

    block->fragments = fragments;
    block->fragment_count = count;
    block->fragmented = 1;

    if (num_fragments) {
        *num_fragments = count;
    }

    return fragments;
}

void compio_free_block(compio_block* block) {
    if (!block) return;
    if (block->data) free(block->data);
    if (block->fragments) free(block->fragments);
    free(block);
}

int compio_set_position(compio_block* block, size_t position) {
    if (!block || position >= block->size) return -1;
    block->position = position;
    return 0;
}

size_t compio_read_from_block(compio_block* block, void* buffer, size_t bytes_to_read) {
    if (!block || !buffer || block->position + bytes_to_read > block->size) {
        return 0;
    }
    memcpy(buffer, (char*)block->data + block->position, bytes_to_read);
    block->position += bytes_to_read;
    return bytes_to_read;
}

size_t compio_write_to_block(compio_block* block, const void* data, size_t bytes_to_write) {
    if (!block || !data || block->position + bytes_to_write > block->size) {
        return 0;
    }
    memcpy((char*)block->data + block->position, data, bytes_to_write);
    block->position += bytes_to_write;
    return bytes_to_write;
}

int compio_add_fragment(compio_block* block, compio_fragment fragment) {
    if (!block) return -1;

    compio_fragment* new_fragments = realloc(block->fragments, sizeof(compio_fragment) * (block->fragment_count + 1));
    if (!new_fragments) return -1;

    block->fragments = new_fragments;
    block->fragments[block->fragment_count] = fragment;
    block->fragment_count++;
    return 0;
}

int compio_remove_fragment(compio_block* block, size_t index) {
    if (!block || index >= block->fragment_count) return -1;

    for (size_t i = index; i < block->fragment_count - 1; i++) {
        block->fragments[i] = block->fragments[i + 1];
    }

    block->fragment_count--;
    if (block->fragment_count == 0) {
        free(block->fragments);
        block->fragments = NULL;
        block->fragmented = 0;
    }
    return 0;
}

int compio_is_fragmented(compio_block* block) {
    return block && block->fragment_count > 0;
}

/* Validates the position within a block */
int compio_validate_position(compio_block* block, size_t position) {
    if (!block || position >= block->size) return -1;
    return 0;
}