#include "infile_object.hpp"

#include "debug_print.hpp"

#include <stdexcept>

#ifdef BM_FILE_OPERATIONS_COUNTER
int n_read_bytes = 0;
int n_written_bytes = 0;

int get_n_read_bytes() {
    return n_read_bytes;
}

int get_n_written_bytes() {
    return n_written_bytes;
}
#endif

static inline bool is_big_endian() {
    uint32_t num = 1;
    return *(reinterpret_cast<unsigned char*>(&num)) == 0;
}

uint64_t lendian_fwrite(const void* ptr, uint64_t size, uint64_t nmemb, FILE* stream) {
    uint64_t ret;
    if (is_big_endian() && size != sizeof(uint8_t)) {
        unsigned char* buffer = new unsigned char[size * nmemb];
        const unsigned char* input = static_cast<const unsigned char*>(ptr);
        if (size == sizeof(uint16_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                buffer[2 * i] = input[2 * i + 1];
                buffer[2 * i + 1] = input[2 * i];
            }
        } else if (size == sizeof(uint32_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                buffer[4 * i] = input[4 * i + 3];
                buffer[4 * i + 1] = input[4 * i + 2];
                buffer[4 * i + 2] = input[4 * i + 1];
                buffer[4 * i + 3] = input[4 * i];
            }
        } else if (size == sizeof(uint64_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                buffer[8 * i] = input[4 * i + 7];
                buffer[8 * i + 1] = input[4 * i + 6];
                buffer[8 * i + 2] = input[4 * i + 5];
                buffer[8 * i + 3] = input[4 * i + 4];
                buffer[8 * i + 4] = input[4 * i + 3];
                buffer[8 * i + 5] = input[4 * i + 2];
                buffer[8 * i + 6] = input[4 * i + 1];
                buffer[8 * i + 7] = input[4 * i];
            }
        } else {
            throw std::invalid_argument("lendian_fwrite possible size values are 1, 2, 4, 8");
        }
        ret = fwrite((void*)buffer, size, nmemb, stream);
        delete buffer;
    } else {
        ret = fwrite(ptr, size, nmemb, stream);
    }

    if (ret != nmemb) {
        WARNING_PRINT("warning: failed to fwrite bytes to file "
                      "(expected: %llu bytes, actual: %llu bytes)\n",
                      size * nmemb, ret * size);
    }
#ifdef BM_FILE_OPERATIONS_COUNTER
    n_written_bytes += ret;
#endif
    return ret;
}

uint64_t lendian_fread(void* ptr, uint64_t size, uint64_t nmemb, FILE* stream) {
    uint64_t ret;
    if (is_big_endian() && size != sizeof(uint8_t)) {
        ret = fread(ptr, size, nmemb, stream);
        unsigned char* output = static_cast<unsigned char*>(ptr);
        if (size == sizeof(uint16_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                std::swap(output[2 * i], output[2 * i + 1]);
            }
        } else if (size == sizeof(uint32_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                std::swap(output[4 * i], output[4 * i + 3]);
                std::swap(output[4 * i + 1], output[4 * i + 2]);
            }
        } else if (size == sizeof(uint64_t)) {
            for (uint32_t i = 0; i < nmemb; i++) {
                std::swap(output[8 * i], output[8 * i + 7]);
                std::swap(output[8 * i + 1], output[8 * i + 6]);
                std::swap(output[8 * i + 2], output[8 * i + 5]);
                std::swap(output[8 * i + 3], output[8 * i + 4]);
            }
        } else {
            throw std::invalid_argument("lendian_fread possible size values are 1, 2, 4, 8");
        }
        return ret;
    } else {
        ret = fread(ptr, size, nmemb, stream);
    }

    if (ret != nmemb) {
        WARNING_PRINT("warning: failed to fread bytes from file "
                      "(expected: %llu bytes, actual: %llu bytes)\n",
                      size * nmemb, ret * size);
    }
#ifdef BM_FILE_OPERATIONS_COUNTER
    n_read_bytes += ret;
#endif
    return ret;
}