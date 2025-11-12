/**
 * @file tree_types.hpp
 * @brief tree_key and tree_val types
 *
 */

#ifndef TREE_TYPES_H_
#define TREE_TYPES_H_

#include <cstdint>

namespace compio {

/**
 * @brief Type for key in btree
 *
 */
typedef struct {
    uint64_t hash; /**< last 64 bits of hashed internal file name */
    uint64_t pos;  /**< Position of block start in uncompressed file */
} tree_key;

/**
 * @brief Type for value in btree
 *
 */
typedef struct {
    uint64_t addr; /**< Address of storage_block in archive file */
    uint64_t size; /**< Original size of uncompressed block */
} tree_val;

/**
 * @brief Less-than comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x < y
 */
inline bool operator<(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos < y.pos;
    return x.hash < y.hash;
}

/**
 * @brief Equality comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x == y
 */
inline bool operator==(const tree_key &x, const tree_key &y) {
    return x.hash == y.hash && x.pos == y.pos;
}

/**
 * @brief Greater-than comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x > y
 */
inline bool operator>(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos > y.pos;
    return x.hash > y.hash;
}

/**
 * @brief Less-than-or-equal comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x <= y
 */
inline bool operator<=(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos <= y.pos;
    return x.hash <= y.hash;
}

/**
 * @brief Greater-than-or-equal comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x >= y
 */
inline bool operator>=(const tree_key &x, const tree_key &y) {
    if (x.hash == y.hash)
        return x.pos >= y.pos;
    return x.hash >= y.hash;
}

/**
 * @brief Inequality comparison for tree keys
 * @param x First key
 * @param y Second key
 * @return True if x != y
 */
inline bool operator!=(const tree_key &x, const tree_key &y) {
    return x.hash != y.hash || x.pos != y.pos;
}

/**
 * @brief Add offset to tree key position
 * @param x Original key
 * @param size Offset to add
 * @return New key with adjusted position
 */
inline tree_key operator+(const tree_key &x, uint64_t size) { return {x.hash, x.pos + size}; }

/**
 * @internal
 * @brief Template for minimum value
 */
template <class T> constexpr T _min();

/**
 * @internal
 * @brief Minimum tree_key value
 */
template <> constexpr tree_key _min<tree_key>() { return {0, 0}; }

template <class T> constexpr T _max();
template <> constexpr tree_key _max<tree_key>() { return {(uint64_t)-1, (uint64_t)-1}; }

struct tree_key_hash {
    size_t operator()(const tree_key &key) const { return key.hash ^ key.pos; }
};

inline bool operator==(const tree_val &x, const tree_val &y) {
    return x.addr == y.addr && x.size == y.size;
}

inline bool operator<(const tree_val &x, const tree_val &y) {
    if (x.addr == y.addr)
        return x.size < y.size;
    return x.addr < y.addr;
}

inline bool operator<=(const tree_val &x, const tree_val &y) {
    if (x.addr == y.addr)
        return x.size <= y.size;
    return x.addr <= y.addr;
}

inline bool operator>=(const tree_val &x, const tree_val &y) {
    if (x.addr == y.addr)
        return x.size >= y.size;
    return x.addr >= y.addr;
}

inline bool operator>(const tree_val &x, const tree_val &y) {
    if (x.addr == y.addr)
        return x.size > y.size;
    return x.addr > y.addr;
}

} // namespace compio

#endif // TREE_TYPES_H_