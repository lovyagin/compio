/**
 * @file sha256.cpp
 * @brief SHA-256 hash implementation
 *
 * Public domain SHA-256 implementation.
 * Based on public domain code by Brad Conte.
 */

#include "compio/sha256.hpp"
#include <cstdio>

namespace compio {

// SHA-256 constants
static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTLEFT(a,b) (((a) << (b)) | ((a) >> (32-(b))))
#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))

#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

SHA256::SHA256() {
    datalen_ = 0;
    bitlen_ = 0;
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
}

void SHA256::transform(const uint8_t *data) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = state_[0];
    b = state_[1];
    c = state_[2];
    d = state_[3];
    e = state_[4];
    f = state_[5];
    g = state_[6];
    h = state_[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void SHA256::update(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        data_[datalen_] = data[i];
        datalen_++;
        if (datalen_ == 64) {
            transform(data_);
            bitlen_ += 512;
            datalen_ = 0;
        }
    }
}

void SHA256::finalize(uint8_t *hash) {
    uint32_t i = datalen_;

    // Pad whatever data is left in the buffer.
    if (datalen_ < 56) {
        data_[i++] = 0x80;
        while (i < 56)
            data_[i++] = 0x00;
    } else {
        data_[i++] = 0x80;
        while (i < 64)
            data_[i++] = 0x00;
        transform(data_);
        memset(data_, 0, 56);
    }

    // Append to the padding the total message's length in bits and transform.
    bitlen_ += datalen_ * 8;
    data_[63] = bitlen_;
    data_[62] = bitlen_ >> 8;
    data_[61] = bitlen_ >> 16;
    data_[60] = bitlen_ >> 24;
    data_[59] = bitlen_ >> 32;
    data_[58] = bitlen_ >> 40;
    data_[57] = bitlen_ >> 48;
    data_[56] = bitlen_ >> 56;
    transform(data_);

    // Since this implementation uses little endian byte ordering and SHA uses big endian,
    // reverse all the bytes when copying the final state to the output hash.
    for (i = 0; i < 4; ++i) {
        hash[i] = (state_[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4] = (state_[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8] = (state_[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (state_[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (state_[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (state_[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (state_[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (state_[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

std::string SHA256::hash_to_string(const uint8_t *hash) {
    char hex_str[65];
    for (size_t i = 0; i < 32; ++i) {
        sprintf(hex_str + i * 2, "%02x", hash[i]);
    }
    hex_str[64] = '\0';
    return std::string(hex_str);
}

std::string SHA256::compute(const uint8_t *data, size_t length) {
    SHA256 sha;
    uint8_t hash[32];
    sha.update(data, length);
    sha.finalize(hash);
    return hash_to_string(hash);
}

} // namespace compio

