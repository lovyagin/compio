/**
 * @file utils.hpp
 * @brief Utility functions
 */

#ifndef UTILS_HEADER_
#define UTILS_HEADER_

#include <cstdint>
#include <cstdio>
#include <string>

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
 * @brief Hashes input string via fnv1a algorithm
 * @param s Input string
 * @return resulting hash
 */
uint64_t fnv1a(const char *s);

/**
 * @brief Hashes binary data via fnv1a algorithm
 * @param data Pointer to binary data
 * @param size Size of data in bytes
 * @return resulting hash
 */
uint64_t fnv1a(const uint8_t *data, size_t size);

/**
 * @brief Hashes binary data via 32-bit fnv1a algorithm (more space-efficient)
 * @param data Pointer to binary data
 * @param size Size of data in bytes
 * @return resulting hash
 */
uint32_t fnv1a_32(const uint8_t *data, size_t size);

/**
 * @brief Checks if file is empty
 * @param file Opened file
 * @return true if file is empty, false otherwise
 */
bool is_file_empty(FILE *file);

/**
 * @brief Calculate checksum of data
 * @param data Pointer to the data
 * @param size Size of the data
 * @return Checksum as a string
 */
std::string calculate_checksum(const uint8_t *data, size_t size);

} // namespace compio

#endif // UTILS_HEADER_