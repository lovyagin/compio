//
// Основной заголовочный файл для публичного API библиотеки
//

#ifndef COMPIO_COMPIO_H
#define COMPIO_COMPIO_H

#include <stdio.h>
#include <errno.h>


/**
 * @brief Структура открытого архива
 * 
 */
typedef struct {
    FILE* file;
    const char* mode;
    // ...
} compio_archive;


/**
 * @brief Структура открытого файла в архиве
 * 
 */
typedef struct {
    compio_archive* archive;
    const char* mode;
    size_t cursor;
    // ...
} compio_file;


/**
 * @brief Открыть файл как архив, и вернуть указатель на структуру архива
 * 
 * @param fp Путь к файлу
 * @param mode Режим (https://en.cppreference.com/w/cpp/io/c/fopen)
 * @return compio_archive* 
 */
compio_archive* compio_open_archive(const char* fp, const char* mode);


/**
 * @brief Открыть файл в архиве для чтения или записи, и вернуть указатель на структура файла
 * 
 * @param a Архив
 * @param fp Путь к файлу внутри архива
 * @param mode Режим (https://en.cppreference.com/w/cpp/io/c/fopen)
 * @return compio_file* 
 */
compio_file* compio_open_file(compio_archive* a, const char* fp, const char* mode);


/**
 * @brief Записать блок данных в место текущего указателя в файле
 * 
 * @param ptr Указатель на блок данных
 * @param size Размер единицы данных
 * @param count Количество единиц данных
 * @param f Файл
 * @return size_t 
 */
size_t compio_write(const void* ptr, size_t size, size_t count, compio_file* f);


/**
 * @brief Прочитать блок данных из места текущего указателя в файле
 * 
 * @param ptr Указатель на свободных блок данных для вывода
 * @param size Размер единицы данных
 * @param count Количество единиц данных
 * @param f Файл
 * @return size_t 
 */
size_t compio_read(void* ptr, size_t size, size_t count, compio_file* f);


#define COMP_SEEK_SET   0
#define COMP_SEEK_CUR   1
#define COMP_SEEK_END   2


/**
 * @brief Переместить указатель в файле на offset байтов, в зависимости от параметра whence
 * Возможные значения whence:
 *  - COMP_SEEK_SET - offset отсчитывается от начала файла
 *  - COMP_SEEK_CUR - offset отсчитывается от текущего положения курсора
 *  - COMP_SEEK_END - offset отсчитывается от конца файла
 * 
 * @param f Файл
 * @param offset Сдвиг в количестве байт 
 * @param whence Точка отсчёта для сдвига
 * @return int
 */
int compio_seek(compio_file* f, long offset, int whence);


/**
 * @brief Вернуть текущее положение курсора в файле
 * 
 * @param f Файл
 * @return long 
 */
long compio_tell(compio_file* f);


/**
 * @brief Закрыть файл и удалить структуру файла
 * 
 * @param f Файл
 * @return int 
 */
int compio_close_file(compio_file* f);


/**
 * @brief Закрыть архив и удалить структуру архива
 * 
 * @param a Архив
 * @return int 
 */
int compio_close_archive(compio_archive* a);


#endif //COMPIO_COMPIO_H
