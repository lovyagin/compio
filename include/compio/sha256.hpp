/**
 * @file sha256.hpp
 * @brief SHA-256 hash implementation
 *
 * Public domain SHA-256 implementation.
 * Based on public domain code by Brad Conte.
 */

#ifndef COMPIO_SHA256_HPP
#define COMPIO_SHA256_HPP

#include <cstdint>
#include <cstring>
#include <string>

namespace compio {

/**
 * @class SHA256
 * @brief SHA-256 cryptographic hash calculator
 *
 * Provides an incremental SHA-256 hash computation interface.
 * Supports both streaming updates and one-shot computation.
 *
 * @par Example:
 * @code
 * SHA256 sha;
 * sha.update(data, length);
 * uint8_t hash[32];
 * sha.finalize(hash);
 *
 * // Or one-shot:
 * std::string hash_hex = SHA256::compute(data, length);
 * @endcode
 *
 * @note SHA256 instances cannot be reused after finalize() - create a new instance for each hash.
 * @note Output hash is 32 bytes (256 bits).
 */
class SHA256 {
public:
    /**
     * @brief Constructs a new SHA256 hasher
     *
     * Initializes the hasher with standard SHA-256 initial state.
     * Ready to accept data via update().
     */
    SHA256();

    /**
     * @brief Updates hash with new data
     *
     * Processes additional data through the SHA-256 algorithm.
     * Can be called multiple times before finalize().
     *
     * @param[in] data Pointer to data to hash. Must not be nullptr if length > 0.
     * @param[in] length Number of bytes to hash. Can be 0 (no-op).
     *
     * @note Data is processed incrementally; order and content must match for consistent results.
     * @note Can be called multiple times; all data is processed cumulatively.
     */
    void update(const uint8_t *data, size_t length);

    /**
     * @brief Finalizes hash computation and outputs result
     *
     * Completes the SHA-256 computation and writes the final 32-byte hash.
     * After calling finalize(), the hasher is in an unusable state and cannot be updated.
     *
     * @param[out] hash Pointer to 32-byte buffer for hash output. Must not be nullptr.
     *
     * @note After finalize(), do not call update() or finalize() again on this instance.
     * @note Output hash is in binary format (raw bytes).
     */
    void finalize(uint8_t *hash);

    /**
     * @brief Converts binary SHA-256 hash to hexadecimal string
     *
     * @param[in] hash Pointer to 32-byte binary hash. Must not be nullptr.
     * @return Hexadecimal representation of hash (64 characters, lowercase).
     *
     * @example
     * uint8_t binary_hash[32];
     * sha.finalize(binary_hash);
     * std::string hex = SHA256::hash_to_string(binary_hash);
     * // Result: "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
     */
    static std::string hash_to_string(const uint8_t *hash);

    /**
     * @brief Computes SHA-256 hash of data in one call
     *
     * Convenience function for single-pass hash computation.
     * Equivalent to creating SHA256, calling update(), finalizing, and converting to string.
     *
     * @param[in] data Pointer to data to hash. Must not be nullptr if length > 0.
     * @param[in] length Number of bytes to hash. Can be 0 (empty data hash).
     * @return Hexadecimal SHA-256 hash string (64 characters, lowercase).
     *
     * @par Example:
     * @code
     * std::string hash_hex = SHA256::compute(data, data_length);
     * @endcode
     *
     * @note For large data or streaming scenarios, use incremental update/finalize instead.
     */
    static std::string compute(const uint8_t *data, size_t length);

private:
    void transform(const uint8_t *data);

    uint8_t data_[64];              ///< Internal buffer for pending data
    uint32_t datalen_;              ///< Number of bytes in internal buffer
    uint64_t bitlen_;               ///< Total number of bits processed
    uint32_t state_[8];             ///< SHA-256 state variables (8 x 32-bit words)
};

} // namespace compio

#endif // COMPIO_SHA256_HPP
