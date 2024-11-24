#include "block.h"
#include "btree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type) {
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (block == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for block.\n");
        return NULL;
    }
    block->size = size;
    block->is_compressed = is_compressed;
    if (compression_type != NULL) {
        block->compression_type = _strdup(compression_type);
    } else {
        block->compression_type = NULL;
    }
    block->data = malloc(size);
    if (block->data == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for block data.\n");
        free(block);
        return NULL;
    }
    return block;
}

void compio_free_block(compio_block* block) {
    if (block == NULL) return;
    free(block->data);
    free(block->compression_type);
    free(block);
}

compio_block_container* compio_create_block_container(size_t total_size) {
    compio_block_container* container = (compio_block_container*)malloc(sizeof(compio_block_container));
    if (container == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for block container.\n");
        return NULL;
    }
    container->total_size = total_size;
    container->block_count = 0;
    container->blocks = (compio_block**)malloc(sizeof(compio_block*) * total_size);
    if (container->blocks == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for blocks array.\n");
        free(container);
        return NULL;
    }
    container->index = btree_create(3);  // Create a B-Tree with a degree of 3 for indexing blocks
    if (container->index == NULL) {
        fprintf(stderr, "Error: Could not create B-Tree for indexing.\n");
        free(container->blocks);
        free(container);
        return NULL;
    }
    return container;
}

void compio_free_block_container(compio_block_container* container) {
    if (container == NULL) return;
    for (size_t i = 0; i < container->block_count; i++) {
        compio_free_block(container->blocks[i]);
    }
    free(container->blocks);
    btree_free(container->index);  // Free the B-Tree index
    free(container);
}

int compio_add_block(compio_block_container* container, compio_block* block) {
    if (container == NULL || block == NULL) return -1;
    if (container->block_count >= container->total_size) {
        fprintf(stderr, "Error: Block container is full.\n");
        return -1;
    }
    container->blocks[container->block_count] = block;
    btree_insert(container->index, container->block_count, block);  // Insert the block into the B-Tree index
    container->block_count++;
    compio_update_index(container);
    return 0;
}

int compio_remove_block(compio_block_container* container, size_t block_index) {
    if (container == NULL || block_index >= container->block_count) return -1;
    compio_free_block(container->blocks[block_index]);
    // Update the B-Tree to remove the block by its index
    btree_delete(container->index, block_index);
    // Shift remaining blocks to fill the gap
    for (size_t i = block_index; i < container->block_count - 1; i++) {
        container->blocks[i] = container->blocks[i + 1];
    }
    container->block_count--;
    compio_update_index(container);
    return 0;
}

compio_block* compio_find_block(compio_block_container* container, size_t position) {
    if (container == NULL) return NULL;
    // Use the B-Tree index to find the block by position
    BTreeNode* node = btree_search(container->index, position);
    if (node == NULL) {
        fprintf(stderr, "Error: Block not found at position %zu.\n", position);
        return NULL;
    }
    return container->blocks[position];
}

void compio_update_index(compio_block_container* container) {
    if (container == NULL) return;
    btree_free(container->index);
    container->index = btree_create(3);  // Recreate the B-Tree index
    for (size_t i = 0; i < container->block_count; i++) {
        btree_insert(container->index, i, container->blocks[i]);
    }
}
