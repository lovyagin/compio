//
// Основной заголовочный файл для публичного API библиотеки
//

#ifndef COMPIO_COMPIO_H
#define COMPIO_COMPIO_H

#include <stdio.h>

typedef struct {
    int mode; // 1 << 0 - r, 1 << 1 - w
    // ...
} compio_archive;

typedef struct {
    int mode; // 1 << 0 - r, 1 << 1 - w
    size_t cursor;
    // ...
} compio_file;

typedef int compio_errcode;

// Открытие сжатого архива
compio_archive* compio_open_archive(const char* fp, const char* mode, compio_errcode* error);

// Открытие файла в архиве
compio_file* compio_open_file(compio_archive* a, const char* fp, const char* mode, compio_errcode* error);

// Запись в файл
size_t compio_write(const void* ptr, size_t size, size_t count, compio_file* f);

// Чтение из файла
size_t compio_read(void* ptr, size_t size, size_t count, compio_file* f);

// Перемещение указателя в файле
void compio_seek(compio_file* f, long offset, int whence, compio_errcode* error);

// Получение текущей позиции указателя в файле
long compio_tell(compio_file* f);

// Закрытие файла в архиве
int compio_close_file(compio_file* f);

// Закрытие архива
int compio_close_archive(compio_archive* a);


#endif //COMPIO_COMPIO_H
