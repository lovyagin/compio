#include "utils.hpp"

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

}