#ifndef COMPIO_BLOCK_H
#define COMPIO_BLOCK_H

#include <stddef.h>

/**
 * @struct compio_block
 * @brief Структура, представляющая блок данных.
 *
 * Эта структура используется для описания блока данных, который может
 * быть сжат или представлен как фрагмент. Включает информацию о позиции,
 * размере и статусе фрагмента.
 */
typedef struct {
    size_t offset;      /**< Смещение начала блока в архиве */
    size_t size;        /**< Размер блока */
    int is_compressed;  /**< Флаг, указывающий, сжат ли блок */
    void* data;         /**< Указатель на данные блока */
} compio_block;



/**
 * @function compio_create_block
 * @brief Создает новый блок данных.
 *
 * Функция выделяет память для нового блока и инициализирует его параметры.
 *
 * @param size Размер блока данных.
 * @param is_compressed Флаг сжатия блока (0 — не сжат, 1 — сжат).
 * @return Указатель на созданный блок.
 */
compio_block* compio_create_block(size_t size, int is_compressed);



/**
 * @struct compio_fragment
 * @brief Структура, представляющая фрагмент, который может быть частью блока.
 *
 * Каждый фрагмент хранит информацию о своем смещении и размере в контексте блока.
 * Используется при разбиении больших блоков на более мелкие фрагменты.
 */
typedef struct {
    size_t offset;      /**< Смещение фрагмента в блоке */
    size_t size;        /**< Размер фрагмента */
} compio_fragment;



// Функции для работы с блоками и фрагментацией

/**
 * @function compio_block_init
 * @brief Инициализация блока данных.
 *
 * Функция инициализирует блок с заданным смещением, размером и флагом сжатия.
 *
 * @param offset Смещение блока данных.
 * @param size Размер блока данных.
 * @param is_compressed Флаг сжатия блока (0 — не сжат, 1 — сжат).
 * @param data Указатель на данные блока.
 * @return Указатель на инициализированный блок.
 */
compio_block* compio_block_init(size_t offset, size_t size, int is_compressed, void* data);



/**
 * @function compio_find_block_by_offset
 * @brief Поиск блока по заданному смещению.
 *
 * Функция ищет блок в массиве блоков по смещению.
 *
 * @param blocks Массив блоков.
 * @param num_blocks Количество блоков в массиве.
 * @param offset Смещение, по которому ищется блок.
 * @return Указатель на найденный блок или NULL, если не найден.
 */
compio_block* compio_find_block_by_offset(compio_block* blocks, size_t num_blocks, size_t offset);



/**
 * @function compio_split_block_into_fragments
 * @brief Разбиение блока на фрагменты.
 *
 * Функция делит блок данных на более мелкие фрагменты заданного размера.
 *
 * @param block Блок, который нужно разбить.
 * @param fragment_size Размер каждого фрагмента.
 * @param num_fragments Количество фрагментов, на которое нужно разбить блок.
 * @return Массив фрагментов, на которые был разделен блок.
 */
compio_fragment* compio_split_block_into_fragments(compio_block* block, size_t fragment_size, size_t* num_fragments);

/**
 * @function compio_free_block
 * @brief Освобождает память, занятую блоком данных.
 *
 * Функция освобождает память, выделенную для блока, и выполняет необходимую
 * очистку.
 *
 * @param block Указатель на блок, который необходимо освободить.
 */
void compio_free_block(compio_block* block);

#endif // COMPIO_BLOCK_H
