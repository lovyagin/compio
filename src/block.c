#include "block.h"
#include <string.h>
#include <stdio.h>

compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type) {
    compio_block* block = malloc(sizeof(compio_block));
    if (!block) {
        fprintf(stderr, "Error: Failed to allocate memory for block.\n");
        return NULL;
    }

    block->offset = 0;
    block->size = size;
    block->is_compressed = is_compressed;
    block->compression_type = compression_type ? _strdup(compression_type) : NULL;
    block->data = malloc(size);
    if (!block->data) {
        fprintf(stderr, "Error: Failed to allocate memory for block data.\n");
        free(block->compression_type);
        free(block);
        return NULL;
    }

    return block;
}

void compio_free_block(compio_block* block) {
    if (!block) return;

    free(block->compression_type);
    free(block->data);
    free(block);
}

compio_block_container* compio_create_block_container(size_t total_size) {
    compio_block_container* container = malloc(sizeof(compio_block_container));
    if (!container) {
        fprintf(stderr, "Error: Failed to allocate memory for block container.\n");
        return NULL;
    }

    container->total_size = total_size;
    container->block_count = 0;
    container->blocks = NULL;
    container->index = NULL;

    return container;
}

int compio_add_block(compio_block_container* container, compio_block* block) {
    if (!container || !block) return -1;

    compio_block** new_blocks = realloc(container->blocks, (container->block_count + 1) * sizeof(compio_block*));
    if (!new_blocks) {
        fprintf(stderr, "Error: Failed to allocate memory for new block in container.\n");
        return -1;
    }

    container->blocks = new_blocks;
    container->blocks[container->block_count] = block;
    container->block_count++;

    return 0;
}

int compio_remove_block(compio_block_container* container, size_t block_index) {
    if (!container || block_index >= container->block_count) return -1;

    compio_free_block(container->blocks[block_index]);

    for (size_t i = block_index; i < container->block_count - 1; i++) {
        container->blocks[i] = container->blocks[i + 1];
    }

    container->block_count--;
    container->blocks = realloc(container->blocks, container->block_count * sizeof(compio_block*));

    return 0;
}

compio_block* compio_find_block(compio_block_container* container, size_t position, size_t* block_offset) {
    if (!container || !block_offset) return NULL;

    for (size_t i = 0; i < container->block_count; i++) {
        if (position >= container->blocks[i]->offset && position < (container->blocks[i]->offset + container->blocks[i]->size)) {
            *block_offset = position - container->blocks[i]->offset;
            return container->blocks[i];
        }
    }

    return NULL;
}

int compio_update_index(compio_block_container* container) {
    // Placeholder for index update logic (e.g., updating a B-tree or other data structure)
    return 0;
}

void compio_free_block_container(compio_block_container* container) {
    if (!container) return;

    for (size_t i = 0; i < container->block_count; i++) {
        compio_free_block(container->blocks[i]);
    }

    free(container->blocks);
    free(container);
}

compio_file_state* compio_open_block_file_state(compio_block_container* container) {
    if (!container) return NULL;

    compio_file_state* state = malloc(sizeof(compio_file_state));
    if (!state) {
        fprintf(stderr, "Error: Failed to allocate memory for file state.\n");
        return NULL;
    }

    state->container = container;
    state->position = 0;
    state->current_block = 0;
    state->block_offset = 0;

    return state;
}

int compio_set_file_position(compio_file_state* state, size_t position) {
    if (!state || position >= state->container->total_size) return -1;

    state->position = position;
    size_t block_offset;
    compio_block* block = compio_find_block(state->container, position, &block_offset);
    if (!block) return -1;

    state->current_block = block - state->container->blocks[0]; // Calculate index from pointer arithmetic
    state->block_offset = block_offset;

    return 0;
}

void compio_free_file_state(compio_file_state* state) {
    if (!state) return;
    free(state);
}
