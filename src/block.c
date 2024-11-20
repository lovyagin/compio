#include "block.h"
#include "btree.h"
#include <string.h>

/* Creates a new block */
compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type) {
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (!block) return NULL;

    block->offset = 0;
    block->size = size;
    block->is_compressed = is_compressed;
    block->compression_type = compression_type ? _strdup(compression_type) : NULL;
    block->data = NULL;

    return block;
}

/* Frees memory associated with a block */
void compio_free_block(compio_block* block) {
    if (block) {
        free(block->compression_type);
        free(block->data);
        free(block);
    }
}

/* Creates a block container */
compio_block_container* compio_create_block_container(size_t total_size) {
    compio_block_container* container = (compio_block_container*)malloc(sizeof(compio_block_container));
    if (!container) return NULL;

    container->total_size = total_size;
    container->block_count = 0;
    container->blocks = NULL;
    container->index = btree_create(2); // Assuming a degree of 2 for simplicity

    return container;
}

/* Adds a block to the container */
int compio_add_block(compio_block_container* container, compio_block* block) {
    if (!container || !block) return -1;

    if (container->block_count % 10 == 0) { // Resize logic
        size_t new_capacity = container->block_count + 10;
        compio_block** new_blocks = realloc(container->blocks, new_capacity * sizeof(compio_block*));
        if (!new_blocks) return -1;

        container->blocks = new_blocks;
    }

    container->blocks[container->block_count] = block;
    container->block_count++;

    // Insert into the B-tree
    btree_insert(container->index, block->offset, block);

    return 0;
}

/* Removes a block from the container */
int compio_remove_block(compio_block_container* container, size_t block_index) {
    if (!container || block_index >= container->block_count) return -1;

    compio_block* block = container->blocks[block_index];

    // Remove from the B-tree
    btree_remove(container->index, block->offset);

    // Free the block
    compio_free_block(block);

    // Shift blocks
    for (size_t i = block_index; i < container->block_count - 1; i++) {
        container->blocks[i] = container->blocks[i + 1];
    }

    container->block_count--;

    return 0;
}

/* Finds a block by position */
compio_block* compio_find_block(compio_block_container* container, size_t position, size_t* block_offset) {
    if (!container || !block_offset) return NULL;

    compio_block* block = btree_search(container->index, position);
    if (block) {
        *block_offset = position - block->offset;
    }

    return block;
}

/* Frees resources of a block container */
void compio_free_block_container(compio_block_container* container) {
    if (container) {
        for (size_t i = 0; i < container->block_count; i++) {
            compio_free_block(container->blocks[i]);
        }
        free(container->blocks);
        btree_free(container->index);
        free(container);
    }
}

/* Opens a file state for managing block access */
compio_file_state* compio_open_block_file_state(compio_block_container* container) {
    if (!container) return NULL;

    compio_file_state* state = (compio_file_state*)malloc(sizeof(compio_file_state));
    if (!state) return NULL;

    state->container = container;
    state->position = 0;
    state->current_block = 0;
    state->block_offset = 0;

    return state;
}

/* Sets the current position in the file */
int compio_set_file_position(compio_file_state* state, size_t position) {
    if (!state || position >= state->container->total_size) return -1;

    state->position = position;

    compio_block* block = compio_find_block(state->container, position, &state->block_offset);
    if (!block) return -1;

    state->current_block = block->offset;
    return 0;
}

/* Frees a file state */
void compio_free_file_state(compio_file_state* state) {
    if (state) {
        free(state);
    }
}
