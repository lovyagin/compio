#ifndef COMPIO_BLOCK_H
#define COMPIO_BLOCK_H

#include <stddef.h>

// Структура, представляющая блок данных
typedef struct {
    size_t offset;      // Смещение блока данных
    size_t size;        // Размер блока данных
    int is_compressed;  // Флаг, указывающий, сжато ли содержимое блока
    void* data;         // Указатель на данные блока
} compio_block;

// Структура, представляющая фрагмент, который может быть частью блока
typedef struct {
    size_t offset;      // Смещение фрагмента в блоке
    size_t size;        // Размер фрагмента
} compio_fragment;

// Функции для работы с блоками и фрагментацией

/**
 * Инициализация блока данных.
 *
 * @param offset Смещение блока данных.
 * @param size Размер блока данных.
 * @param is_compressed Флаг сжатия блока.
 * @param data Указатель на данные блока.
 * @return Указатель на инициализированный блок.
 */
compio_block* compio_block_init(size_t offset, size_t size, int is_compressed, void* data);

/**
 * Поиск блока по заданному смещению.
 *
 * @param blocks Массив блоков.
 * @param num_blocks Количество блоков в массиве.
 * @param offset Смещение, по которому ищется блок.
 * @return Указатель на найденный блок или NULL, если не найден.
 */
compio_block* compio_find_block_by_offset(compio_block* blocks, size_t num_blocks, size_t offset);

/**
 * Разбиение блока на фрагменты.
 *
 * @param block Блок, который нужно разбить.
 * @param fragment_size Размер каждого фрагмента.
 * @param num_fragments Количество фрагментов, на которое нужно разбить блок.
 * @return Массив фрагментов, на которые был разделен блок.
 */
compio_fragment* compio_split_block_into_fragments(compio_block* block, size_t fragment_size, size_t* num_fragments);

/**
 * Освобождение памяти, занятой блоком.
 *
 * @param block Указатель на блок, который нужно освободить.
 */
void compio_block_free(compio_block* block);

#endif // COMPIO_BLOCK_H
