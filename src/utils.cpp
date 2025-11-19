#include "compio/utils.hpp"

#include <cstdio>
#include <cstring>

#include "compio/allocator.hpp"

namespace compio {

uint8_t parse_mode(const char *mode) {
    uint8_t mode_b = 0;
    switch (mode[0]) {
    case 'r':
        mode_b |= mode_bit::r;
        break;
    case 'w':
        mode_b |= mode_bit::w;
        break;
    case 'a':
        mode_b |= mode_bit::a;
        break;
    default:
        return 0;
    }

    switch (mode[1]) {
    case '+':
        mode_b |= mode_bit::plus;
        break;
    case 0:
        break;
    default:
        return 0;
    }

    return mode_b;
}

uint64_t fnv1a(const char *s) {
    uint64_t hash = 0xcbf29ce484222325;
    while (*s)
        hash = (hash ^ *s++) * 0x100000001b3;
    return hash;
}

bool is_file_empty(FILE *file) {
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    return fsize == 0;
}

} // namespace compio