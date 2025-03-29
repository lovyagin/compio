#include "compio.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    compio_config config;
    compio_build_default_config(&config);
    config.b_tree_degree = 3;
    config.block_size = 8;
    
    if (!strcmp(argv[1], "read")) {
        compio_archive* archive = compio_open_archive(argv[2], "r", &config);
        
        if (!archive) {
            printf("failed to open archive, errno=%d\n", errno);
        }

        compio_file* file = compio_open_file("fileA", archive);
    
        if (!file) {
            printf("failed to open file, errno=%d\n", errno);
        }
        
        char data[17];
        char test[sizeof(data)];
        uint64_t bytes = compio_read(test, sizeof(data), file);
    
        printf("read %d bytes\n", bytes);

        for (int i = 0; i < bytes; ++i)
            printf("%llu, ", test[i]);
        printf("\n");
    
        compio_close_file(file);
        compio_close_archive(archive);
    } else {
        compio_archive* archive = compio_open_archive(argv[2], "w+", &config);
        
        if (!archive) {
            printf("failed to open archive, errno=%d\n", errno);
        }

        compio_file* file = compio_open_file("fileA", archive);
    
        if (!file) {
            printf("failed to open file, errno=%d\n", errno);
        }

        char data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        uint64_t bytes = compio_write(data, sizeof(data), file);

        printf("written %llu bytes\n", bytes);
        
        compio_close_file(file);
        compio_close_archive(archive);
    }

    return 0;
}
