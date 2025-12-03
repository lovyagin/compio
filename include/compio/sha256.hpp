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

class SHA256 {
public:
    SHA256();
    void update(const uint8_t *data, size_t length);
    void finalize(uint8_t *hash);

    static std::string hash_to_string(const uint8_t *hash);
    static std::string compute(const uint8_t *data, size_t length);

private:
    void transform(const uint8_t *data);

    uint8_t data_[64];
    uint32_t datalen_;
    uint64_t bitlen_;
    uint32_t state_[8];
};

} // namespace compio

#endif // COMPIO_SHA256_HPP
