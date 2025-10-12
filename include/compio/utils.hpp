/**
 * @file utils.hpp
 * @brief Utility functions and operators for the compression library
 */

#ifndef UTILS_HEADER_
#define UTILS_HEADER_

#include "compio/compio_file.hpp"
#include "compio.h"

namespace compio {

/**
 * @brief File mode bits for parsing fopen-style mode strings
 */
enum mode_bit { r = 0b0001, w = 0b0010, a = 0b0100, plus = 0b1000 };

/**
 * @brief Parse fopen-style mode string into bit flags
 * @param mode Mode string (e.g., "r", "w+", "a")
 * @return Parsed mode as bit flags
 */
uint8_t parse_mode(const char *mode);

/**
 * @brief Less-than comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x < y
 */
inline bool operator<(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos < y.pos;
    return x.hash < y.hash;
}

/**
 * @brief Equality comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x == y
 */
inline bool operator==(const tree_key &x, const tree_key &y) {
    return x.hash == y.hash && x.pos == y.pos;
}

/**
 * @brief Greater-than comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x > y
 */
inline bool operator>(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos > y.pos;
    return x.hash > y.hash;
}

/**
 * @brief Less-than-or-equal comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x <= y
 */
inline bool operator<=(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos <= y.pos;
    return x.hash <= y.hash;
}

/**
 * @brief Greater-than-or-equal comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x >= y
 */
inline bool operator>=(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos >= y.pos;
    return x.hash >= y.hash;
}

/**
 * @brief Inequality comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x != y
 */
inline bool operator!=(const tree_key &x, const tree_key &y) {
    return x.hash != y.hash || x.pos != y.pos;
}

/**
 * @brief Add offset to tree key position
 * @param x Original key
 * @param size Offset to add
 * @return New key with adjusted position
 */
inline tree_key operator+(const tree_key &x, uint64_t size) { return {x.hash, x.pos + size}; }

/**
 * @internal
 * @brief Template for minimum value
 */
template <class T> constexpr T _min();

/**
 * @internal
 * @brief Minimum tree_key value
 */
template <> constexpr tree_key _min<tree_key>() { return {0, 0}; }

template <class T> constexpr T _max();
template <> constexpr tree_key _max<tree_key>() { return {(uint64_t)-1, (uint64_t)-1}; }

uint64_t fnv1a(const char *s);

bool is_file_empty(FILE *file);

} // namespace compio

#endif // UTILS_HEADER_