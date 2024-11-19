#include "block.h"
#include <string.h>
#include <stdio.h>

/* Helper function to initialize a B-tree (placeholder for actual implementation) */
static void* btree_create() {
    // Replace with actual B-tree initialization
    return NULL;
}

/* Helper function to insert into a B-tree (placeholder) */
static int btree_insert(void* tree, size_t key, compio_block* value) {
    // Replace with actual B-tree insertion logic
    return 0;
}

/* Helper function to find in a B-tree (placeholder) */
static compio_block* btree_find(void* tree, size_t key) {
    // Replace with actual B-tree lookup logic
    return NULL;
}

/* Helper function to free a B-tree (placeholder) */
static void btree_free(void* tree) {
    // Replace with actual B-tree cleanup
}

/* Block management functions */
compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type) {
    compio_block* block = malloc(sizeof(compio_block));
    if (!block) return NULL;

    block->offset = 0;
    block->size = size;
    block->is_compressed = is_compressed;
    block->compression_type = compression_type ? strdup(compression_type) : NULL;
    block->data = malloc(size);
    if (!block->data) {
        if (block->compression_type) free(block->compression_type);
        free(block);
        return NULL;
    }

    return block;
}

void compio_free_block(compio_block* block) {
    if (!block) return;
    if (block->data) free(block->data);
    if (block->compression_type) free(block->compression_type);
    free(block);
}

/* Block container management functions */
compio_block_container* compio_create_block_container(size_t total_size) {
    compio_block_container* container = malloc(sizeof(compio_block_container));
    if (!container) return NULL;

    container->total_size = total_size;
    container->block_count = 0;
    container->blocks = NULL;
    container->index = btree_create();
    if (!container->index) {
        free(container);
        return NULL;
    }

    return container;
}

int compio_add_block(compio_block_container* container, compio_block* block) {
    if (!container || !block) return -1;

    compio_block** new_blocks = realloc(container->blocks, sizeof(compio_block*) * (container->block_count + 1));
    if (!new_blocks) return -1;

    container->blocks = new_blocks;
    container->blocks[container->block_count] = block;
    container->block_count++;

    if (btree_insert(container->index, block->offset, block) != 0) {
        return -1;  // B-tree insertion failed
    }

    return 0;
}

int compio_remove_block(compio_block_container* container, size_t block_index) {
    if (!container || block_index >= container->block_count) return -1;

    compio_block* block_to_remove = container->blocks[block_index];

    for (size_t i = block_index; i < container->block_count - 1; i++) {
        container->blocks[i] = container->blocks[i + 1];
    }

    container->block_count--;
    container->blocks = realloc(container->blocks, sizeof(compio_block*) * container->block_count);
    if (container->block_count > 0 && !container->blocks) {
        return -1;  // Memory reallocation failed
    }

    compio_free_block(block_to_remove);

    return 0;
}

compio_block* compio_find_block(compio_block_container* container, size_t position, size_t* block_offset) {
    if (!container || position >= container->total_size) return NULL;

    compio_block* block = btree_find(container->index, position);
    if (!block) return NULL;

    if (block_offset) {
        *block_offset = position - block->offset;
    }

    return block;
}

int compio_update_index(compio_block_container* container) {
    if (!container) return -1;

    // Clear and rebuild the B-tree
    btree_free(container->index);
    container->index = btree_create();
    if (!container->index) return -1;

    for (size_t i = 0; i < container->block_count; i++) {
        if (btree_insert(container->index, container->blocks[i]->offset, container->blocks[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

void compio_free_block_container(compio_block_container* container) {
    if (!container) return;

    for (size_t i = 0; i < container->block_count; i++) {
        compio_free_block(container->blocks[i]);
    }

    if (container->blocks) free(container->blocks);
    if (container->index) btree_free(container->index);

    free(container);
}

/* File state management functions */
compio_file_state* compio_open_file(compio_block_container* container) {
    if (!container) return NULL;

    compio_file_state* state = malloc(sizeof(compio_file_state));
    if (!state) return NULL;

    state->container = container;
    state->position = 0;
    state->current_block = 0;
    state->block_offset = 0;

    return state;
}

int compio_set_file_position(compio_file_state* state, size_t position) {
    if (!state || position >= state->container->total_size) return -1;

    size_t block_offset;
    compio_block* block = compio_find_block(state->container, position, &block_offset);
    if (!block) return -1;

    state->position = position;
    state->current_block = block - state->container->blocks[0];
    state->block_offset = block_offset;

    return 0;
}

void compio_free_file_state(compio_file_state* state) {
    if (!state) return;
    free(state);
}
