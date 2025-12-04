/**
 * @file checksum_alternatives.cpp
 * @brief Alternative checksum implementations for comparison and future use
 *
 * This file contains alternative checksum algorithms that were considered
 * but not currently used in production. Kept for:
 * - Performance comparisons
 * - Future enhancements
 * - Testing different trade-offs between speed and collision resistance
 */

#include <cstdint>
#include <string>
#include "compio/sha256.hpp"

namespace compio {
namespace checksum_alternatives {

/**
 * @brief Calculate SHA-256 checksum (cryptographically strong, 32 bytes)
 * @param data Pointer to data
 * @param size Size of data in bytes
 * @return SHA-256 hash as hex string (64 characters)
 *
 * Pros:
 * - Cryptographically secure
 * - Extremely low collision probability
 * Cons:
 * - 64 bytes overhead per block (hex representation)
 * - Slower computation
 *
 * Overhead for 4KB block: ~1.56%
 */
std::string sha256_checksum(const uint8_t *data, size_t size) {
    if (!data || size == 0) {
        return std::string(64, '0');
    }
    return SHA256::compute(data, size);
}

/**
 * @brief Verify SHA-256 checksum
 * @param data Pointer to data
 * @param size Size of data
 * @param expected Expected checksum (64 hex chars)
 * @return true if checksum matches
 */
bool verify_sha256_checksum(const uint8_t *data, size_t size, const char *expected) {
    if (!data || size == 0) {
        return true;
    }
    std::string computed = SHA256::compute(data, size);
    return strncmp(expected, computed.c_str(), 64) == 0;
}

/**
 * @brief Calculate FNV-1a 64-bit checksum (fast, 8 bytes)
 * @param data Pointer to data
 * @param size Size of data in bytes
 * @return 64-bit hash
 *
 * Pros:
 * - Very fast
 * - Good distribution
 * - Only 8 bytes overhead
 * Cons:
 * - Not cryptographically secure
 * - Higher collision probability than SHA-256
 *
 * Overhead for 4KB block: ~0.20%
 */
uint64_t fnv1a_64_checksum(const uint8_t *data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x100000001b3;
    }
    return hash;
}

/**
 * @brief Calculate FNV-1a 32-bit checksum (fastest, 4 bytes) - CURRENTLY USED
 * @param data Pointer to data
 * @param size Size of data in bytes
 * @return 32-bit hash
 *
 * Pros:
 * - Extremely fast
 * - Minimal overhead (4 bytes)
 * - Sufficient for detecting random corruption
 * Cons:
 * - Not cryptographically secure
 * - Higher collision probability than 64-bit version
 *
 * Overhead for 4KB block: ~0.10%
 *
 * This is the current production implementation used in storage_block.
 */
uint32_t fnv1a_32_checksum(const uint8_t *data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    uint32_t hash = 0x811c9dc5;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x01000193;
    }
    return hash;
}

/**
 * @brief Comparison table of checksum algorithms:
 *
 * Algorithm    | Size (bytes) | Speed      | Collision Resistance | Overhead (4KB block)
 * -------------|--------------|------------|---------------------|---------------------
 * SHA-256      | 64 (hex)     | Slow       | Cryptographic       | 1.56%
 * FNV-1a 64    | 8            | Very Fast  | Good                | 0.20%
 * FNV-1a 32    | 4            | Fastest    | Acceptable          | 0.10% (CURRENT)
 *
 * For our use case (detecting accidental corruption, not malicious attacks),
 * FNV-1a 32-bit provides the best balance of speed and space efficiency.
 */

} // namespace checksum_alternatives
} // namespace compio

