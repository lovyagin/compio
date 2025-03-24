#include "compio.h"

#include <stdio.h>

int main(int argc, char *argv[]) {
    compio_config config;
    compio_build_default_config(&config);
    config.b_tree_degree = 3;
    config.block_size = 8;

    compio_archive* archive = compio_open_archive("test.archive", "w+", &config);
    compio_file* file = compio_open_file("fileA", archive);
    
    char data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    compio_write(data, sizeof(data), file);

    compio_close_file(file);
    compio_close_archive(archive);

    archive = compio_open_archive("test.archive", "r", &config);
    file = compio_open_file("fileA", archive);

    char test[sizeof(data)];
    compio_read(test, sizeof(data), file);

    for (int i = 0; i < sizeof(data); ++i)
        printf("%d, ", test[i]);
    printf("\n");

    compio_close_file(file);
    compio_close_archive(archive);

    return 0;
}
