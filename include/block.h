//
// Позиционирование несжатых блоков, внутренняя фрагментация
//

#ifndef COMPIO_BLOCK_H
#define COMPIO_BLOCK_H

#include <stddef.h>
#include <stdlib.h>
#include "stdbool.h"



/**
 * @struct compio_fragment
 * @brief Структура, представляющая фрагмент блока.
 *
 * Описывает фрагмент, который является частью несжатого блока.
 */
typedef struct {
    size_t offset;      /**< Смещение фрагмента в блоке */
    size_t size;        /**< Размер фрагмента */
    int is_active;      /**< Флаг, указывающий, используется ли фрагмент */
} compio_fragment;



/**
 * @struct compio_block
 * @brief Структура, представляющая блок данных.
 *
 * Описывает несжатый или сжатый блок данных, его размер, смещение и состояние.
 * Также включает информацию о фрагментах, если блок разбит.
 */
typedef struct {
    size_t offset;      /**< Смещение начала блока в архиве */
    size_t size;        /**< Размер блока */
    int is_compressed;  /**< Флаг, указывающий, сжат ли блок */
    void* data;         /**< Указатель на данные блока */
    size_t position;    /**< Текущая позиция чтения/записи внутри блока */
    int fragmented;     /**< Флаг, указывающий, фрагментирован ли блок */
    compio_fragment* fragments; /**< Указатель на массив фрагментов, если блок фрагментирован */
    size_t fragment_count;  /**< Количество фрагментов в массиве */
} compio_block;



/**
 * @function compio_create_block
 * @brief Создает новый блок данных.
 *
 * Функция выделяет память для нового блока и инициализирует его параметры.
 *
 * @param size Размер блока д��нных.
 * @param is_compressed Флаг сжатия блока (0 — не сжат, 1 — сжат).
 * @return Указатель на созданный блок.
 */
compio_block* compio_create_block(size_t size, int is_compressed);



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



/**
 * @function compio_set_position
 * @brief Устанавливает позицию внутри несжатого блока.
 *
 * @param block Указатель на блок данных.
 * @param position Новая позиция ��нутри блока.
 * @return 0, если успешно; -1, если позиция некорректна.
 */
int compio_set_position(compio_block* block, size_t position);



/**
 * @function compio_read_from_block
 * @brief Читает данные из несжатого блока с текущей позиции.
 *
 * @param block Указатель на блок данных.
 * @param buffer Буфер для записи прочитанных данных.
 * @param bytes_to_read Количество байт для чтения.
 * @return Количество реально прочитанных байт.
 */
size_t compio_read_from_block(compio_block* block, void* buffer, size_t bytes_to_read);



/**
 * @function compio_write_to_block
 * @brief Записывает данные в несжатый блок с текущей позиции.
 *
 * @param block Указатель на блок данных.
 * @param data Данные для записи.
 * @param bytes_to_write Количество байт для записи.
 * @return Количество реально записанных байт.
 */
size_t compio_write_to_block(compio_block* block, const void* data, size_t bytes_to_write);



/**
 * @function compio_add_fragment
 * @brief Добавляет фрагмент в блок.
 *
 * @param block Указатель на структуру блока.
 * @param fragment Структура фрагмента, который необходимо добавить.
 * @return 0 при успешном добавлении; -1 при ошибке (например, при нехватке памяти).
 */
int compio_add_fragment(compio_block* block, compio_fragment fragment);



/**
 * @function compio_remove_fragment
 * @brief Удаляет фрагмент из блока.
 *
 * @param block Указатель на структуру блока.
 * @param index Индекс удаляемого фрагмента (начиная с 0).
 * @return 0 при успешном удалении; -1 при ошибке (например, если индекс некорректен).
 */
int compio_remove_fragment(compio_block* block, size_t index);



/**
 * @function compio_is_fragmented
 * @brief Проверяет, является ли блок фрагментированным.
 *
 * @param block Указатель на структуру блока.
 * @return true, если блок фрагментирован; false, если нет.
 */
int compio_is_fragmented(compio_block* block);



/**
 * @function compio_validate_position
 * @brief Проверяет корректность позиции внутри блока.
 *
 * @param block Указатель на структуру блока.
 * @param position Позиция для проверки.
 * @return 0, если позиция допустима; -1, если позиция выходит за пределы размера блока.
 */
int compio_validate_position(compio_block* block, size_t position);

#endif // COMPIO_BLOCK_H