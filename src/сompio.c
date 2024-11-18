//
// Основные функции API
//

#include "compio.h"


compio_archive* compio_open_archive(const char* fp, const char* mode) {
    compio_archive* archive = (compio_archive*)malloc(sizeof(compio_archive));
    if (!archive) {
        // errno is set by malloc
        // https://man7.org/linux/man-pages/man3/malloc.3.html#top_of_page:~:text=On%20error%2C%20these%20functions%20return%20NULL%20and%20set%20errno.
        return NULL;
    }

    archive->file = fopen(fp, mode);
    if (!archive->file) {
        // errno is set by fopen
        // https://en.cppreference.com/w/cpp/io/c/fopen#:~:text=POSIX%20requires%20that%20errno%20is%20set%20in%20this%20case.
        free(archive);
        return NULL;
    }

    archive->mode = mode;
    return archive;
}