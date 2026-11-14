#pragma once

#include <cerrno>
#include <cstdint>
#include <map>

#include "compio.h"

namespace compio {

/**
 * @brief Compressors an archive can encode and decode with
 *
 * New blocks are packed with the active compressor. A block records the compression_type of the
 * compressor that packed it, and is unpacked by the compressor registered under that type; the
 * built-in algorithms are always present, an external one has to be registered.
 * Callers hold the archive lock: changes need it exclusively, lookups shared.
 */
class compressor_set {
    std::map<uint32_t, compio_compressor> by_type_;
    const compio_compressor *active_ = nullptr;

public:
    compressor_set() {
        for (const auto type : {COMPIO_COMPRESS_DUMMY, COMPIO_COMPRESS_ZLIB, COMPIO_COMPRESS_LZ4,
                                COMPIO_COMPRESS_ZSTD, COMPIO_COMPRESS_BROTLI}) {
            compio_compressor built;
            if (compio_build_compressor_by_type(&built, type) == COMPIO_SUCCESS) {
                by_type_.emplace(static_cast<uint32_t>(type), built);
            }
        }
    }

    compressor_set(const compressor_set &) = delete;
    compressor_set &operator=(const compressor_set &) = delete;

    /** @brief Whether the compressor has everything a block needs from it */
    static bool is_usable(const compio_compressor &c) {
        return c.compress && c.decompress && c.get_bufsize && c.compression_type > 0;
    }

    /** @brief Register a compressor for decoding blocks that carry its type */
    bool add(const compio_compressor &c) {
        if (!is_usable(c)) {
            errno = EINVAL;
            return false;
        }
        by_type_.insert_or_assign(static_cast<uint32_t>(c.compression_type), c);
        return true;
    }

    /** @brief Register a compressor and pack new blocks with it */
    bool activate(const compio_compressor &c) {
        if (!add(c)) return false;
        active_ = &by_type_.at(static_cast<uint32_t>(c.compression_type));
        return true;
    }

    const compio_compressor *active() const { return active_; }

    /** @brief The compressor that decodes blocks of this type, or null when it is unknown */
    const compio_compressor *find(uint32_t type) const {
        const auto found = by_type_.find(type);
        if (found != by_type_.end()) return &found->second;
        errno = ENOTSUP;
        return nullptr;
    }
};

} // namespace compio
