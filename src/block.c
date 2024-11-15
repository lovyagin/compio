#include <stdio.h>
#include "block.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Создает новый блок данных, выделяя память для него.
 */
compio_block* compio_create_block(size_t size, int is_compressed) {
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (block == NULL) {
        return NULL;
    }
    block->offset = 0;
    block->size = size;
    block->is_compressed = is_compressed;
    block->data = malloc(size);
    if (block->data == NULL) {
        free(block);
        return NULL;
    }
    return block;
}

/**
 * @brief Инициализирует блок с заданными параметрами.
 */
compio_block* compio_block_init(size_t offset, size_t size, int is_compressed, void* data) {
    compio_block* block = (compio_block*)malloc(sizeof(compio_block));
    if (block == NULL) {
        return NULL;
    }
    block->offset = offset;
    block->size = size;
    block->is_compressed = is_compressed;
    block->data = malloc(size);
    if (block->data == NULL) {
        free(block);
        return NULL;
    }
    memcpy(block->data, data, size);
    return block;
}

/**
 * @brief Поиск блока по заданному смещению.
 */
compio_block* compio_find_block_by_offset(compio_block* blocks, size_t num_blocks, size_t offset) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (blocks[i].offset == offset) {
            return &blocks[i];
        }
    }
    return NULL;
}

/**
 * @brief Разбивает блок данных на фрагменты фиксированного размера.
 */
compio_fragment* compio_split_block_into_fragments(compio_block* block, size_t fragment_size, size_t* num_fragments) {
    if (fragment_size == 0 || block == NULL || block->size == 0) {
        *num_fragments = 0;
        return NULL;
    }

    *num_fragments = (block->size + fragment_size - 1) / fragment_size;
    compio_fragment* fragments = (compio_fragment*)malloc(*num_fragments * sizeof(compio_fragment));
    if (fragments == NULL) {
        *num_fragments = 0;
        return NULL;
    }

    for (size_t i = 0; i < *num_fragments; i++) {
        fragments[i].offset = block->offset + i * fragment_size;
        fragments[i].size = (i == *num_fragments - 1) ? block->size - i * fragment_size : fragment_size;
    }

    return fragments;
}

/**
 * @brief Освобождает память, выделенную для блока данных.
 */
void compio_free_block(compio_block* block) {
    if (block != NULL) {
        free(block->data);
        free(block);
    }
}