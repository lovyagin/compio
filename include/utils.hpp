#ifndef UTILS_HEADER_
#define UTILS_HEADER_

#include "compio.h"
#include "compio_file.hpp"

namespace compio {

const uint8_t read_bit = 0b001;
const uint8_t write_bit = 0b010;
const uint8_t append_bit = 0b100;

uint8_t parse_mode(const char* mode);

void flush_header(compio_archive* archive);

tree_key operator+(tree_key x, uint64_t size);

tree_key get_key(const char* fname, uint64_t pos);

} // namespace compio

#endif // UTILS_HEADER_