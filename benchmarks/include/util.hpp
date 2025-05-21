#ifndef BENCHMARK_UTIL_HPP_
#define BENCHMARK_UTIL_HPP_

#include <sys/stat.h>


inline unsigned long get_file_size(const char* filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

#endif // BENCHMARK_UTIL_HPP_