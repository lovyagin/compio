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
 * @brief Continues calculating 32-bit fnv1a hash
 * @param hash Initial hash value
 * @param data Pointer to binary data
 * @param size Size of data in bytes
 * @return resulting hash
 */
uint32_t fnv1a_32_continue(uint32_t hash, const uint8_t *data, size_t size);

/**
 * @brief Portable 64-bit file seek
 *
 * Uses _fseeki64 on Windows (where long is 32-bit) and fseeko on POSIX
 * to correctly handle file offsets beyond 2GB.
 *
 * @param file File pointer
 * @param offset 64-bit offset
 * @param whence Seek origin (SEEK_SET, SEEK_CUR, SEEK_END)
 * @return 0 on success, non-zero on error
 */
int fseek64(FILE *file, int64_t offset, int whence);

/**
 * @brief Portable 64-bit file tell
 * @param file File pointer
 * @return Current file position or -1 on error
 */
int64_t ftell64(FILE *file);

/**
 * @brief Checks if file is empty
 * @param file Opened file
 * @return true if file is empty, false otherwise
 */
bool is_file_empty(FILE *file);

/**
 * @brief Check if system is Big Endian
 * @return true if Big Endian, false if Little Endian
 */
inline bool is_big_endian() {
    uint32_t num = 1;
    return *(reinterpret_cast<unsigned char *>(&num)) == 0;
}

/**
 * @brief Swap 64-bit integer endianness
 * @param val Pointer to value to swap
 */
inline void swap_uint64(uint64_t* val) {
    uint8_t* p = (uint8_t*)val;
    uint8_t temp;
    for(int i=0; i<4; ++i) {
        temp = p[i];
        p[i] = p[7-i];
        p[7-i] = temp;
    }
}

/**
 * @brief Swap 32-bit integer endianness
 * @param val Pointer to value to swap
 */
inline void swap_uint32(uint32_t* val) {
    uint8_t* p = (uint8_t*)val;
    uint8_t temp;
    for(int i=0; i<2; ++i) {
        temp = p[i];
        p[i] = p[3-i];
        p[3-i] = temp;
    }
}

/**
 * @brief Calculate checksum of data
 * @param data Pointer to the data
 * @param size Size of the data
 * @return Checksum as a string
 */
std::string calculate_checksum(const uint8_t *data, size_t size);

} // namespace compio

#endif // UTILS_HEADER_