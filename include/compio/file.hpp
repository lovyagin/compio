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
 * @brief Files table for archive header
 *
 */
struct files_table {
    uint64_t n_files;
    uint32_t max_files;
    uint64_t next_file_id;
    struct file {
        char name[COMPIO_FNAME_MAX_SIZE];
        uint64_t size;
        uint64_t file_id;
    };
    std::vector<file> files;
    
    files_table();
    explicit files_table(uint32_t max_files);
    files_table(const files_table& other); // Custom copy constructor to skip index map
    files_table& operator=(const files_table& other); // Custom assignment operator
    files_table(files_table&& other) noexcept; // Custom move constructor
    files_table& operator=(files_table&& other) noexcept; // Custom move assignment operator

    const file *find(const char *name) const;
    file *find(const char *name);
    file *add(const char *name, bool allow_resize = true);
    int remove(const char *name);
    
    // Rebuild the index_map from the current files vector
    void rebuild_index();

    // Read from disk (external storage)
    // capacity: number of slots allocated on disk
    // n_files: number of used slots
    bool read_from(FILE *file, uint64_t addr, uint32_t capacity, uint64_t n_files);

    // Write to disk (external storage)
    void write_to(FILE *file, uint64_t addr) const;

private:
    // Hash map for fast O(1) file lookups by name.
    // Maps filename (std::string_view) to index in 'files' vector.
    // Transient (not serialized to disk), rebuilt on load/add/remove.
    // Key points to storage inside 'files' vector, so pointers must be stable.
    // 'files' vector may resize (reallocating storage), invalidating pointers.
    // When that happens, rebuild_index() MUST be called to update this map.
    // USING CUSTOM FLAT MAP for memory efficiency and startup speed.
    detail::flat_map index_map_;
};

/**
 * @brief Archive file header
 *
 * The on-disk size of the header is variable: it depends on @c ftable.max_files,
 * which is stored as the first field of the files table.  Use @c disk_size()
 * to obtain the actual byte count rather than @c sizeof(header).
 */
struct header : public infile_object {
    int32_t magic_number; /**< Constant bytes, file signature */
    uint64_t index_root;  /**< Address of B-Tree root in file */
    uint64_t file_size;
    files_table ftable; /**< Files table */
    uint64_t allocator_state_offset;
    uint64_t allocator_state_size;
    uint32_t compression_type; /**< Type of compression algorithm used */
    uint32_t block_size;       /**< Size of data blocks */
    uint32_t b_tree_degree;    /**< Degree of B-Tree index */
    uint64_t sequence_id;      /**< Monotonic counter for double-buffering updates */
    uint64_t files_table_addr;     /**< Address of dynamic files table (v5+) */
    uint32_t files_table_capacity; /**< Capacity of dynamic files table (v5+) */
    uint8_t checksum[32];      /**< SHA-256 checksum of the header (excluding this field) */

    /**
     * @brief Construct default header
     *
     */
    header();
    explicit header(uint32_t max_files);

    bool read_from(FILE *file, uint64_t addr) override;
    void write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;

    /** @brief On-disk size of this header */
    uint64_t disk_size() const;

    /** @brief Size reserved for headers (double buffering implies 2x disk_size) */
    uint64_t reserved_size() const { return disk_size() * 2; }

    /** @brief Calculate SHA-256 checksum of the header content */
    void compute_checksum(uint8_t *out_hash) const;

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
    static constexpr uint8_t signature = 67;
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
    void write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;
    void validate() const;
};

/**
 * @brief Size of index node metadata (without arrays)
 */
#define INDEX_NODE_METASIZE                                                                        \
    (sizeof(uint8_t) /* signature */ + sizeof(index_node::is_leaf) + sizeof(index_node::num_keys))
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
    static constexpr uint8_t signature = 171; // FNV-1a (v1, no back-ref)
    static constexpr uint8_t signature_crc32c = 172; // CRC32C (v1, no back-ref)
    // v2 self-describing blocks: carry their owning {hash,pos} key, enabling
    // index reconstruction from blocks alone after partial/total index loss.
    static constexpr uint8_t signature_backref = 173; // FNV-1a (v2, back-ref)
    static constexpr uint8_t signature_backref_crc32c = 174; // CRC32C (v2, back-ref)

    uint8_t is_compressed;           /**< Is this block compressed */
    uint64_t size;                   /**< Size of data array */
    uint64_t original_size;          /**< Original size (size of uncompressed data) */
    std::unique_ptr<uint8_t[]> data; /**< Data block */
    uint32_t checksum;               /**< Checksum (4 bytes) */

    compio_checksum_type checksum_type; /**< Algorithm used for checksum */

    /** @brief Owning B-tree key (v2 only). Written into the block, read back on
     * recovery. has_backref tells whether src_key is present on disk. */
    tree_key src_key{};
    bool has_backref = false;

    /** @brief On-disk metadata size implied by a block signature byte */
    static uint64_t meta_size_for(uint8_t sig);

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
    void write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager = nullptr) const override;

    /**
     * @brief Calculate and store SHA-256 checksum of the data
     */
    void calculate_checksum();

    /**
     * @brief Verify data integrity against stored checksum
     * @return true if checksum matches, false otherwise
     */
    bool verify_checksum() const;

    /** @brief Compute checksum over (back-ref key if v2) + data */
    uint32_t compute_checksum() const;
};

/**
 * @brief Size of v1 storage block metadata (without data, no back-ref)
 */
#define STORAGE_BLOCK_METASIZE                                                                     \
    (sizeof(uint8_t) /* signature */ + sizeof(storage_block::is_compressed) +                      \
     sizeof(storage_block::size) + sizeof(storage_block::original_size) +                          \
     sizeof(uint32_t) /* FNV-1a 32-bit checksum */)

/**
 * @brief Size of v2 storage block metadata (back-ref {hash,pos} added).
 * New blocks are always written in v2 layout.
 */
#define STORAGE_BLOCK_METASIZE_V2                                                                  \
    (STORAGE_BLOCK_METASIZE + sizeof(tree_key) /* hash + pos */)

} // namespace compio

#endif // COMPIO_FILE_H_