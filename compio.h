//
// Основной заголовочный файл для публичного API библиотеки
//

#ifndef COMPIO_COMPIO_H
#define COMPIO_COMPIO_H

#include <stdio.h>


#define COMP_MODE_READ      (1 << 0)
#define COMP_MODE_WRITE     (1 << 1)


/**
 * @brief Структура открытого архива
 * 
 */
typedef struct {
    int mode; // 1 << 0 - r, 1 << 1 - w
    // ...
} compio_archive;


/**
 * @brief Структура открытого файла в архиве
 * 
 */
typedef struct {
    int mode; // 1 << 0 - r, 1 << 1 - w
    size_t cursor;
    // ...
} compio_file;

typedef int compio_errcode;


/**
 * @brief Открыть файл как архив, и вернуть указатель на структуру архива
 * 
 * @param fp 
 * @param mode 
 * @param error 
 * @return compio_archive* 
 */
compio_archive* compio_open_archive(const char* fp, const char* mode, compio_errcode* error);


/**
 * @brief Открыть файл в архиве для чтения или записи, и вернуть указатель на структура файла
 * 
 * @param a 
 * @param fp 
 * @param mode 
 * @param error 
 * @return compio_file* 
 */
compio_file* compio_open_file(compio_archive* a, const char* fp, const char* mode, compio_errcode* error);


/**
 * @brief Записать блок данных в место текущего указателя в файле
 * 
 * @param ptr 
 * @param size 
 * @param count 
 * @param f 
 * @return size_t 
 */
size_t compio_write(const void* ptr, size_t size, size_t count, compio_file* f);


/**
 * @brief Прочитать блок данных из места текущего указателя в файле
 * 
 * @param ptr 
 * @param size 
 * @param count 
 * @param f 
 * @return size_t 
 */
size_t compio_read(void* ptr, size_t size, size_t count, compio_file* f);


#define COMP_SEEK_SET   (1 << 0)
#define COMP_SEEK_CUR   (1 << 1)
#define COMP_SEEK_END   (1 << 2)


/**
 * @brief Переместить указатель в файле на offset байтов, в зависимости от параметра whence
 * Возможные значения whence:
 *  - COMP_SEEK_SET - offset отсчитывается от начала файла
 *  - COMP_SEEK_CUR - offset отсчитывается от текущего положения курсора
 *  - COMP_SEEK_END - offset отсчитывается от конца файла
 * 
 * @param f 
 * @param offset 
 * @param whence 
 * @param error 
 */
void compio_seek(compio_file* f, long offset, int whence, compio_errcode* error);


/**
 * @brief Вернуть текущее положение курсора в файле
 * 
 * @param f 
 * @return long 
 */
long compio_tell(compio_file* f);


/**
 * @brief Закрыть файл и удалить структуру файла
 * 
 * @param f 
 * @return int 
 */
int compio_close_file(compio_file* f);


/**
 * @brief Закрыть архив и удалить структуру архива
 * 
 * @param a 
 * @return int 
 */
int compio_close_archive(compio_archive* a);


#endif //COMPIO_COMPIO_H
