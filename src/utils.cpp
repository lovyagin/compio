#include "compio/utils.hpp"

#include <cstdio>
#include <cstring>

#include "compio/allocator.hpp"

#include "third_party/hash_sha256.h"

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

uint64_t get_hash_tail(const char *fname) {
    hash_sha256 hash;
    hash.sha256_init();
    hash.sha256_update((const uint8_t *)fname, COMPIO_FNAME_MAX_SIZE);
    auto hashed_fname = hash.sha256_final();
    uint64_t hash_tail;

    memcpy(&hash_tail, hashed_fname.data(), sizeof(uint64_t));
    memcpy(&hash_tail, &(*hashed_fname.begin()), sizeof(uint64_t));

    return hash_tail;
}

} // namespace compio