/**
 * @file file.hpp
 * @brief Archive file navigation
 *
 */

#ifndef COMPIO_FILE_H_
#define COMPIO_FILE_H_

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <string>
#include <string_view>

#include "compio/infile_object.hpp"
#include "compio/tree_types.hpp"
#include "compio/sha256.hpp"
#include "compio/detail/flat_map.hpp"
#include "compio.h"

namespace compio {

/**
 * @brief Files table: name, size and id of every logical file
 *
 * On disk the table is a sequence of variable-length records
 * `name_len (u16) | size (u64) | file_id (u64) | name (name_len bytes)`, little
 * endian, without padding or free slots. It is always rewritten as a whole to a
 * new place (copy-on-write), so records never need room to grow in place.
 */
struct files_table {
    struct file {
        std::string name;
        uint64_t size = 0;
        uint64_t file_id = 0;
    };

    /** Bytes of a record before its name */
    static constexpr uint64_t record_header_size = sizeof(uint16_t) + 2 * sizeof(uint64_t);

    uint64_t next_file_id = 1;
    std::vector<file> files; /**< Live entries only */

    files_table() = default;
    explicit files_table(uint32_t expected_files);
    files_table(const files_table& other);
    files_table& operator=(const files_table& other);
    files_table(files_table&& other) noexcept = default;
    files_table& operator=(files_table&& other) noexcept = default;

    const file *find(const char *name) const;
    file *find(const char *name);
    file *add(const char *name);
    int remove(const char *name);

    /** @brief Exact on-disk size of the table */
    uint64_t serialized_size() const;
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Parse a serialized table, rejecting malformed records and
     *        duplicate names or ids
     */
    bool parse(const uint8_t *data, uint64_t size, uint64_t count);

    /**
     * @brief Stored form of a serialized table: the bytes as they are
     *        (encoding 0) or, when that is shorter, their plain size (8 bytes)
     *        followed by the bytes packed with the built-in compressor
     *        `preferred`. A custom compressor is never used: the table must be
     *        readable before any compressor is registered.
     * @param[out] encoding 0 or the compression type that was used
     */
    static std::vector<uint8_t> encode(const std::vector<uint8_t> &plain, uint32_t preferred, uint32_t &encoding);

    /** @brief Whether a header may name this encoding */
    static bool is_known_encoding(uint32_t encoding);

    /**
     * @brief Read, verify (CRC32C of the stored bytes), decode and parse the table
     */
    bool read_from(FILE *file, uint64_t addr, uint64_t size, uint64_t count, uint32_t checksum,
                   uint32_t encoding);
    bool write_to(FILE *file, uint64_t addr) const;

private:
    void rebuild_index();

    // Keys point into the names stored in 'files'. Moving a std::string can move
    // its characters (small-string storage), so the map is rebuilt whenever the
    // vector reallocates.
    detail::flat_map index_map_;
};

/**
 * @brief Archive file header (format v7, fixed size)
 */
struct header : public infile_object {
    int32_t magic_number; /**< Constant bytes, file signature and format version */
    uint64_t index_root;  /**< Address of B-Tree root in file */
    uint64_t file_size;
    files_table ftable; /**< Files table, stored at files_table_addr */
    uint64_t allocator_state_offset;
    uint64_t allocator_state_size;
    uint32_t compression_type; /**< Type of compression algorithm used */
    uint32_t block_size;       /**< Size of data blocks */
    uint32_t b_tree_degree;    /**< Degree of B-Tree index */
    uint64_t sequence_id;      /**< Monotonic counter for double-buffering updates */
    uint64_t files_table_addr;     /**< Address of the files table (0 when empty) */
    uint64_t files_table_size;     /**< Size of the files table in bytes */
    uint32_t files_table_checksum; /**< CRC32C of the files table */
    uint32_t files_table_encoding; /**< 0: the table is stored as is; otherwise the built-in compression type that packed it */
    uint8_t checksum[32];      /**< SHA-256 checksum of the header (excluding this field) */

    /** @brief On-disk size of a header slot */
    static constexpr uint64_t disk_size_bytes =
        4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 8 + 8 + 8 + 4 + 4;

    header();
    explicit header(uint32_t expected_files);

    bool read_from(FILE *file, uint64_t addr) override;
    bool write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;

    /** Write the header, reporting seek errors and incomplete writes. */
    bool write_checked(FILE *file, uint64_t addr) const;

    /** @brief On-disk size of this header */
    uint64_t disk_size() const { return disk_size_bytes; }

    /** @brief Size reserved for headers (double buffering implies 2x disk_size) */
    uint64_t reserved_size() const { return disk_size() * 2; }

    /** @brief Calculate SHA-256 checksum of the header fields for a table of n_files */
    void compute_checksum(uint8_t *out_hash, uint64_t n_files) const;

    /** 
     * @brief Read header and validate checksum without asserting
     * @return true if valid, false if corrupted
     */
    bool load_and_validate(FILE *file, uint64_t addr);
};

/**
 * @brief Key for B-Tree index entries
 *
 * Uniquely identifies a data block by combining filename hash
 * and position in the uncompressed file.
 */
// struct tree_key {
//     uint64_t hash; /**< 64-bit hash of the internal filename */
//     uint64_t pos;  /**< Starting byte offset in uncompressed file */
// };

/**
 * @brief Type for value in btree
 *
 */
// typedef struct {
//     uint64_t addr; /**< Address of storage_block in archive file */
//     uint64_t size; /**< Original size of uncompressed block */
// } tree_val;

/**
 * @brief B-Tree (index) node
 *
 */
struct index_node : public infile_object {
    static constexpr uint8_t signature = 68;
    uint8_t is_leaf;                /**< Is this node a leaf */
    uint32_t num_keys;              /**< Number of used keys in node */
    std::vector<tree_key> keys;     /**< Blocks start positions in uncompressed file */
    std::vector<tree_val> values;   /**< Storage blocks addresses in archive file */
    std::vector<uint64_t> children; /**< Children addresses in archive file */
    std::vector<int64_t>
        key_additions; /**< Additions for keys pos in children (used in segment operations) */

    int tree_degree; /**< B-Tree degree (not saved in file) */

    /**
     * @brief Construct empty index node
     *
     * @param tree_degree degree of b-tree
     */
    index_node(int tree_degree);

    bool read_from(FILE *file, uint64_t addr) override;
    bool write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;
    void validate() const;
};

/**
 * @brief Size of index node metadata (without arrays)
 */
#define INDEX_NODE_METASIZE                                                                        \
    (sizeof(uint8_t) /* signature */ + sizeof(index_node::is_leaf) + sizeof(index_node::num_keys) +   \
     sizeof(uint32_t) /* CRC32C of the preceding bytes */)
/**
 * @brief Whole size of index node
 */
#define INDEX_NODE_SIZE(degree)                                                                    \
    (INDEX_NODE_METASIZE + sizeof(tree_key) * (2 * degree - 1) +                                   \
     sizeof(tree_val) * (2 * degree - 1) + sizeof(uint64_t) * (2 * degree) +                       \
     sizeof(int64_t) * (2 * degree))

/**
 * @brief Block of (usually compressed) data
 *
 */
struct storage_block : public infile_object {
    static constexpr uint8_t signature = 175;        // FNV-1a checksums
    static constexpr uint8_t signature_crc32c = 176; // CRC32C checksums

    /** @brief Codec of a block that is stored as is (the compression_type values name the others) */
    static constexpr uint32_t codec_stored = 0;

    uint32_t codec;                  /**< codec_stored or the compression_type that packed the data */
    uint64_t size;                   /**< Size of data array */
    uint64_t original_size;          /**< Original size (size of uncompressed data) */
    std::unique_ptr<uint8_t[]> data; /**< Data block */
    uint32_t checksum;               /**< Checksum of the owning key and the stored bytes */
    uint32_t data_checksum;          /**< Checksum of the uncompressed bytes; 0 for a stored block */

    compio_checksum_type checksum_type; /**< Algorithm used for checksums */

    /** @brief Owning B-tree key, kept in the block so that the index can be rebuilt from blocks alone */
    tree_key src_key{};

    bool is_compressed() const { return codec != codec_stored; }

    storage_block();

    storage_block(std::unique_ptr<uint8_t[]> &&data, uint64_t size);

    /**
     * @brief Construct storage block with data of size
     *
     * @param size size of data
     */
    storage_block(uint64_t size);

    /** 
     * @brief Read block from file and validate content
     * @return true if valid, false if corrupted
     */
    bool read_from(FILE *file, uint64_t addr) override;
    bool write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;

    /**
     * @brief Calculate and store SHA-256 checksum of the data
     */
    void calculate_checksum();

    /**
     * @brief Verify data integrity against stored checksum
     * @return true if checksum matches, false otherwise
     */
    bool verify_checksum() const;

    /** @brief Checksum over the owning key and the stored bytes */
    uint32_t compute_checksum() const;

    /** @brief Checksum of the uncompressed bytes */
    static uint32_t compute_data_checksum(compio_checksum_type type, const uint8_t *plain, uint64_t plain_size);
};

/**
 * @brief Size of storage block metadata (without data)
 */
#define STORAGE_BLOCK_METASIZE                                                                     \
    (sizeof(uint8_t) /* signature */ + sizeof(uint32_t) /* codec */ +                              \
     sizeof(uint64_t) /* size */ + sizeof(uint64_t) /* original size */ +                          \
     sizeof(compio::tree_key) /* owning key */ + 2 * sizeof(uint32_t) /* checksums */)

} // namespace compio

#endif // COMPIO_FILE_H_
