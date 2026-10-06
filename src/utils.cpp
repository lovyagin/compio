#include "compio/utils.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <intrin.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <linux/falloc.h>
#endif

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#endif

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

#include "compio/allocator.hpp"
#include "compio/debug_print.hpp"

namespace compio {

uint8_t parse_mode(const char *mode) {
    if (!mode || mode[0] == '\0') return 0;
    
    uint8_t mode_b = 0;
    switch (mode[0]) {
    case 'r': mode_b |= mode_bit::r; break;
    case 'w': mode_b |= mode_bit::w; break;
    case 'a': mode_b |= mode_bit::a; break;
    default: return 0;
    }

    // Handle remaining chars
    for (int i = 1; mode[i] != '\0'; ++i) {
        if (mode[i] == '+') {
            mode_b |= mode_bit::plus;
        } else if (mode[i] == 'b') {
            // ignore binary flag
        } else {
            return 0; // Invalid character
        }
    }
    return mode_b;
}

uint64_t fnv1a(const char *s) {
    uint64_t hash = 0xcbf29ce484222325;
    while (*s)
        hash = (hash ^ *s++) * 0x100000001b3;
    return hash;
}

uint64_t fnv1a(const uint8_t *data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x100000001b3;
    }
    return hash;
}

uint32_t fnv1a_32(const uint8_t *data, size_t size) {
    uint32_t hash = 0x811c9dc5;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x01000193;
    }
    return hash;
}

uint32_t fnv1a_32_continue(uint32_t hash, const uint8_t *data, size_t size) {
    uint32_t h = hash;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0x01000193;
    }
    return h;
}

static uint32_t crc32c_table[256];
static std::once_flag crc32c_flag;

static void init_crc32c_table() {
    uint32_t poly = 0x82F63B78;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
        crc32c_table[i] = crc;
    }
}

static uint32_t crc32c_sw(uint32_t crc, const uint8_t *data, size_t size) {
    std::call_once(crc32c_flag, init_crc32c_table);
    
    uint32_t c = ~crc;
    for (size_t i = 0; i < size; i++) {
        c = (c >> 8) ^ crc32c_table[(c ^ data[i]) & 0xFF];
    }
    return ~c;
}

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    #pragma GCC push_options
    #pragma GCC target("sse4.2")
    #include <nmmintrin.h>
    #pragma GCC pop_options
    
    #define COMPIO_HAVE_SSE42 1
    
    __attribute__((target("sse4.2")))
    static uint32_t crc32c_hw(uint32_t crc, const uint8_t *data, size_t size) {
        uint64_t c = ~crc;
        const uint8_t *p = data;
        
        // Align to 8 bytes (optional optimization, but good for u64)
        while (size > 0 && ((uintptr_t)p & 7) != 0) {
            c = _mm_crc32_u8((uint32_t)c, *p++);
            size--;
        }
        
        // Process 64-bit chunks
        while (size >= 8) {
            uint64_t chunk;
            std::memcpy(&chunk, p, sizeof(chunk));
            c = _mm_crc32_u64(c, chunk);
            p += 8;
            size -= 8;
        }
        
        // Process remaining bytes
        while (size > 0) {
            c = _mm_crc32_u8((uint32_t)c, *p++);
            size--;
        }
        
        return ~(uint32_t)c;
    }

#elif defined(_MSC_VER)
    #define COMPIO_HAVE_SSE42 1
    
    static uint32_t crc32c_hw(uint32_t crc, const uint8_t *data, size_t size) {
        // MSVC's _mm_crc32_u64 takes unsigned __int64
        unsigned __int64 c = ~static_cast<unsigned __int64>(crc);
        const uint8_t *p = data;
        
        #if defined(_M_X64) || defined(_M_AMD64)
        // Process 64-bit chunks (x64 only)
        // Align to 8 bytes
        while (size > 0 && ((uintptr_t)p & 7) != 0) {
            c = _mm_crc32_u8((unsigned int)c, *p++);
            size--;
        }
        
        while (size >= 8) {
            // MSVC intrinsics handle unaligned loads on x64 usually, but memcpy is safer or explicit cast if we trust alignment
            // However, we aligned p above.
            c = _mm_crc32_u64(c, *(const unsigned __int64*)p);
            p += 8;
            size -= 8;
        }
        #else
        // x86 fallback to 32-bit chunks
        while (size >= 4) {
            c = _mm_crc32_u32((unsigned int)c, *(const unsigned int*)p);
            p += 4;
            size -= 4;
        }
        #endif
        
        // Process remaining bytes
        while (size > 0) {
            c = _mm_crc32_u8((unsigned int)c, *p++);
            size--;
        }
        
        return ~(uint32_t)c;
    }
#endif

uint32_t crc32c_continue(uint32_t crc, const uint8_t *data, size_t size) {
#if defined(COMPIO_HAVE_SSE42)
    #if defined(__GNUC__) || defined(__clang__)
    // Runtime check for SSE4.2 support
    if (__builtin_cpu_supports("sse4.2")) {
        return crc32c_hw(crc, data, size);
    }
    #elif defined(_MSC_VER)
    // Runtime check for SSE4.2 support on MSVC
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    if (cpuInfo[2] & (1 << 20)) { // SSE4.2 bit is 20 in ECX
        return crc32c_hw(crc, data, size);
    }
    #endif
#endif
    return crc32c_sw(crc, data, size);
}

uint32_t crc32c(const uint8_t *data, size_t size) {
    return crc32c_continue(0, data, size);
}

bool is_file_empty(FILE *file) {
    fseek64(file, 0, SEEK_END);
    int64_t fsize = ftell64(file);
    return fsize == 0;
}

uint64_t hole_granularity(FILE *file) {
#if defined(__linux__)
    struct stat st;
    return (fstat(fileno(file), &st) == 0 && st.st_blksize > 0) ? static_cast<uint64_t>(st.st_blksize) : 4096;
#elif defined(__APPLE__) && defined(F_PUNCHHOLE)
    struct statfs fs;
    return (fstatfs(fileno(file), &fs) == 0 && fs.f_bsize > 0) ? static_cast<uint64_t>(fs.f_bsize) : 4096;
#elif defined(_WIN32)
    // NTFS releases the storage of a sparse file in units of 64 KiB.
    UNUSED(file);
    return 65536;
#else
    UNUSED(file);
    return 0;
#endif
}

bool punch_file_hole(FILE *file, uint64_t offset, uint64_t size) {
    if (size == 0) return true;
#if defined(__linux__)
    return fallocate(fileno(file), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                     static_cast<off_t>(size)) == 0;
#elif defined(__APPLE__) && defined(F_PUNCHHOLE)
    // The request must cover whole filesystem blocks, so the range is shrunk to
    // the blocks that lie inside it.
    const uint64_t unit = hole_granularity(file);
    const uint64_t start = (offset + unit - 1) / unit * unit;
    const uint64_t end = (offset + size) / unit * unit;
    if (start >= end) return true;
    fpunchhole_t request{};
    request.fp_offset = static_cast<off_t>(start);
    request.fp_length = static_cast<off_t>(end - start);
    return fcntl(fileno(file), F_PUNCHHOLE, &request) == 0;
#elif defined(_WIN32)
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) return false;
    // Zeroing a range releases its storage only in a file marked as sparse.
    DWORD returned = 0;
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) return false;
    FILE_ZERO_DATA_INFORMATION range;
    range.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
    range.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(offset + size);
    return DeviceIoControl(handle, FSCTL_SET_ZERO_DATA, &range, sizeof(range), nullptr, 0, &returned, nullptr) != 0;
#else
    UNUSED(file);
    UNUSED(offset);
    return false;
#endif
}

int fseek64(FILE *file, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(file, offset, whence);
#else
    return fseeko(file, static_cast<off_t>(offset), whence);
#endif
}

int64_t ftell64(FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}

} // namespace compio