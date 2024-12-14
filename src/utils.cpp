#include "utils.hpp"
#include "third_party/hash_sha256.h"
#include <cstring>

namespace compio {

uint8_t parse_mode(const char* mode) {
    uint8_t mode_b = 0;
    switch (mode[0]) {
    case 'r':
        mode_b |= read_bit;
        break;
    case 'w':
        mode_b |= write_bit;
        break;
    case 'a':
        mode_b |= read_bit | write_bit | append_bit;
        break;
    default:
        return 0;
    }

    switch (mode[1]) {
    case '+':
        mode_b |= read_bit | write_bit;
        break;
    case 0:
        break;
    default:
        return 0;
    }

    return mode_b;
}

void flush_header(compio_archive* archive) { archive->header->write(archive->file); }

tree_key operator+(tree_key x, uint64_t size) {
    return {x.first, x.second + size};
}

tree_key get_key(const char* fname, uint64_t pos) {
    hash_sha256 hash;
    hash.sha256_init();
    hash.sha256_update((const uint8_t*)fname, COMPIO_FNAME_MAX_SIZE);
    auto hashed_fname = hash.sha256_final();
    uint64_t hash_tail;
    memcpy(&hash_tail, hashed_fname.begin(), sizeof(uint64_t));
    return {hash_tail, pos};
}

}