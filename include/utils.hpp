#ifndef UTILS_HEADER_
#define UTILS_HEADER_

#include "compio.h"
#include "compio_file.hpp"

namespace compio {

enum mode_bit { r = 0b0001, w = 0b0010, a = 0b0100, plus = 0b1000 };

uint8_t parse_mode(const char* mode);

tree_key operator+(tree_key x, uint64_t size);

bool operator<(const tree_key& x, const tree_key& y);

bool operator>(const tree_key& x, const tree_key& y);

bool operator<=(const tree_key& x, const tree_key& y);

bool operator>=(const tree_key& x, const tree_key& y);

bool operator==(const tree_key& x, const tree_key& y);

bool operator!=(const tree_key& x, const tree_key& y);

template <class T> constexpr T _min();
template <> constexpr tree_key _min<tree_key>() { return {0, 0}; }

template <class T> constexpr T _max();
template <> constexpr tree_key _max<tree_key>() { return {(uint64_t)-1, (uint64_t)-1}; }

uint64_t get_hash_tail(const char* fname);

} // namespace compio

#endif // UTILS_HEADER_