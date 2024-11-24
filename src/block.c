#include "block.h"
#include "btree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type) { // Allocate memory and create a new block
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (block == NULL) {
        return NULL; // Failed to allocate memory for the block
    }
    block->size = size;
    block->is_compressed = is_compressed;
    if (compression_type != NULL) {
        block->compression_type = _strdup(compression_type); // Using strdup for better portability
    } else {
        block->compression_type = NULL;
    }
    block->data = malloc(size);
    if (block->data == NULL) {
        free(block); // Free block if data allocation fails
        return NULL;
    }
    return block;
}

void compio_free_block(compio_block* block) { // Free memory associated with a block
    if (block == NULL) return;
    free(block->data);
    free(block->compression_type);
    free(block);
}

compio_block_container* compio_create_block_container(size_t total_size) { // Allocate memory and initialize block container
    compio_block_container* container = (compio_block_container*)malloc(sizeof(compio_block_container));
    if (container == NULL) {
        return NULL; // Failed to allocate memory for the container
    }
    container->total_size = total_size;
    container->block_count = 0;
    container->blocks = (compio_block**)malloc(sizeof(compio_block*) * total_size);
    if (container->blocks == NULL) {
        free(container); // Free container if blocks allocation fails
        return NULL;
    }
    container->index = btree_create(3); // Create B-Tree with degree 3
    if (container->index == NULL) {
        free(container->blocks);
        free(container);
        return NULL;
    }
    return container;
}

void compio_free_block_container(compio_block_container* container) { // Free all blocks and container memory
    if (container == NULL) return;
    for (size_t i = 0; i < container->block_count; i++) {
        compio_free_block(container->blocks[i]);
    }
    free(container->blocks);
    btree_free(container->index); // Free the B-Tree index
    free(container);
}

int compio_add_block(compio_block_container* container, compio_block* block) { // Add a block and update B-Tree
    if (container == NULL || block == NULL) return -1;
    if (container->block_count >= container->total_size) {
        fprintf(stderr, "Error: Block container is full.\n");
        return -1; // Container is full
    }
    container->blocks[container->block_count] = block;
    btree_insert(container->index, container->block_count, block); // Insert only the new block
    container->block_count++;
    return 0;
}

int compio_remove_block(compio_block_container* container, size_t block_index) { // Remove a block and update B-Tree more efficiently
    if (container == NULL || block_index >= container->block_count) return -1;
    compio_free_block(container->blocks[block_index]);
    // Update the B-Tree to remove the block by its index
    btree_delete(container->index, block_index); // Efficiently delete block from B-Tree without full rebuild
    // Shift remaining blocks to fill the gap
    for (size_t i = block_index; i < container->block_count - 1; i++) {
        container->blocks[i] = container->blocks[i + 1];
        btree_insert(container->index, i, container->blocks[i]); // Update B-Tree for shifted blocks
    }
    container->block_count--;
    return 0;
}

compio_block* compio_find_block(compio_block_container* container, size_t position) { // Find a block by position using B-Tree index
    if (container == NULL || position >= container->block_count) {
        return NULL; // Position is out of range
    }
    BTreeNode* node = btree_search(container->index, position);
    if (node == NULL) {
        return NULL; // Block not found in B-Tree
    }
    return container->blocks[position];
}

void compio_update_index(compio_block_container* container) { // Rebuild the B-Tree index
    if (container == NULL) return;
    btree_free(container->index); // Free old index
    container->index = btree_create(3); // Recreate B-Tree index
    for (size_t i = 0; i < container->block_count; i++) {
        btree_insert(container->index, i, container->blocks[i]);
    }
}
