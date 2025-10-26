/**
 * @file utils.hpp
 * @brief Utility functions and operators for the compression library
 */

#ifndef UTILS_HEADER_
#define UTILS_HEADER_

#include <cstdint>
#include <cstdio>

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

uint64_t fnv1a(const char *s);

bool is_file_empty(FILE *file);

} // namespace compio

#endif // UTILS_HEADER_