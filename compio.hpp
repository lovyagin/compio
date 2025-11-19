/**
 * @file compio.hpp
 * @brief C++ API wrapper for the compression library.
 *
 * This header provides a modern C++ interface with RAII resource management,
 * stream-style I/O operators, and type-safe operations.
 */

#ifndef COMPIO_HPP
#define COMPIO_HPP

#include "compio.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace compio {

/**
 * @brief Exception thrown on compio errors
 */
class Exception : public std::runtime_error {
public:
    explicit Exception(const std::string& msg) : std::runtime_error(msg) {}
};

/**
 * @brief RAII wrapper for compio_config
 */
class Config {
public:
    Config() {
        compio_build_default_config(&config_);
    }

    Config& set_compressor(const compio_compressor& comp) {
        config_.compressor = comp;
        return *this;
    }

    Config& set_btree_degree(int degree) {
        config_.b_tree_degree = degree;
        return *this;
    }

    Config& set_block_size(int size) {
        config_.block_size = size;
        return *this;
    }

    Config& set_cache_nodes(int size) {
        config_.cache_size__nodes = size;
        return *this;
    }

    Config& set_cache_blocks(int size) {
        config_.cache_size__blocks = size;
        return *this;
    }

    Config& set_allocation_strategy(compio_allocation_strategy strategy) {
        config_.allocation_strategy = strategy;
        return *this;
    }

    Config& set_fill_holes(bool fill) {
        config_.fill_holes_with_zeros = fill;
        return *this;
    }

    Config& set_fragmentation_threshold(uint8_t threshold) {
        config_.fragmentation_threshold = threshold;
        return *this;
    }

    const compio_config* get() const { return &config_; }

private:
    compio_config config_;
};

// Forward declaration
class Archive;

/**
 * @brief RAII wrapper for compio_file with stream-like interface
 */
class File {
public:
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept : file_(other.file_) {
        other.file_ = nullptr;
    }

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            file_ = other.file_;
            other.file_ = nullptr;
        }
        return *this;
    }

    ~File() {
        close();
    }

    /**
     * @brief Write binary data to file
     */
    uint64_t write(const void* data, uint64_t size) {
        if (!file_) throw Exception("File is not open");
        return compio_write(data, size, file_);
    }

    /**
     * @brief Read binary data from file
     */
    uint64_t read(void* data, uint64_t size) {
        if (!file_) throw Exception("File is not open");
        return compio_read(data, size, file_);
    }

    /**
     * @brief Write typed data (binary)
     */
    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, File&>::type
    write_binary(const T& value) {
        uint64_t written = write(&value, sizeof(T));
        if (written != sizeof(T)) {
            throw Exception("Failed to write complete data");
        }
        return *this;
    }

    /**
     * @brief Read typed data (binary)
     */
    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, File&>::type
    read_binary(T& value) {
        uint64_t bytes_read = read(&value, sizeof(T));
        if (bytes_read != sizeof(T)) {
            throw Exception("Failed to read complete data");
        }
        return *this;
    }

    /**
     * @brief Stream output operator for trivially copyable types (binary write)
     */
    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, File&>::type
    operator<<(const T& value) {
        return write_binary(value);
    }

    /**
     * @brief Stream input operator for trivially copyable types (binary read)
     */
    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, File&>::type
    operator>>(T& value) {
        return read_binary(value);
    }

    /**
     * @brief Seek to position in file
     */
    void seek(int64_t offset, uint8_t origin = COMPIO_SEEK_SET) {
        if (!file_) throw Exception("File is not open");
        if (compio_seek(file_, offset, origin) != 0) {
            throw Exception("Seek failed");
        }
    }

    /**
     * @brief Get current position in file
     */
    uint64_t tell() const {
        if (!file_) throw Exception("File is not open");
        return compio_tell(file_);
    }

    /**
     * @brief Check if file is open
     */
    bool is_open() const {
        return file_ != nullptr;
    }

    /**
     * @brief Close the file explicitly
     */
    void close() {
        if (file_) {
            compio_close_file(file_);
            file_ = nullptr;
        }
    }

private:
    friend class Archive;

    explicit File(compio_file* file) : file_(file) {
        if (!file_) throw Exception("Failed to open file");
    }

    compio_file* file_;
};

/**
 * @brief RAII wrapper for compio_archive
 */
class Archive {
public:
    /**
     * @brief Open archive with default config
     */
    Archive(const std::string& filepath, const std::string& mode) {
        Config default_config;
        archive_ = compio_open_archive(filepath.c_str(), mode.c_str(), default_config.get());
        if (!archive_) {
            throw Exception("Failed to open archive: " + filepath);
        }
    }

    /**
     * @brief Open archive with custom config
     */
    Archive(const std::string& filepath, const std::string& mode, const Config& config) {
        archive_ = compio_open_archive(filepath.c_str(), mode.c_str(), config.get());
        if (!archive_) {
            throw Exception("Failed to open archive: " + filepath);
        }
    }

    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    Archive(Archive&& other) noexcept : archive_(other.archive_) {
        other.archive_ = nullptr;
    }

    Archive& operator=(Archive&& other) noexcept {
        if (this != &other) {
            close();
            archive_ = other.archive_;
            other.archive_ = nullptr;
        }
        return *this;
    }

    ~Archive() {
        close();
    }

    /**
     * @brief Open file within archive
     */
    File open_file(const std::string& name) {
        if (!archive_) throw Exception("Archive is not open");
        compio_file* file = compio_open_file(name.c_str(), archive_);
        return File(file);
    }

    /**
     * @brief Remove file from archive
     */
    void remove_file(const std::string& name) {
        if (!archive_) throw Exception("Archive is not open");
        if (compio_remove_file(archive_, name.c_str()) != 0) {
            throw Exception("Failed to remove file: " + name);
        }
    }

    /**
     * @brief Flush all cached data to filesystem
     */
    void flush() {
        if (!archive_) throw Exception("Archive is not open");
        compio_flush(archive_);
    }

    /**
     * @brief Check if archive is open
     */
    bool is_open() const {
        return archive_ != nullptr;
    }

    /**
     * @brief Close the archive explicitly
     */
    void close() {
        if (archive_) {
            compio_close_archive(archive_);
            archive_ = nullptr;
        }
    }

private:
    compio_archive* archive_;
};

/**
 * @brief Helper functions for building compressors
 */
namespace compressors {
    inline compio_compressor dummy() {
        compio_compressor comp;
        compio_build_dummy_compressor(&comp);
        return comp;
    }

    inline compio_compressor zlib() {
        compio_compressor comp;
        compio_build_zlib_compressor(&comp);
        return comp;
    }

    inline compio_compressor lz4() {
        compio_compressor comp;
        compio_build_lz4_compressor(&comp);
        return comp;
    }

    inline compio_compressor zstd() {
        compio_compressor comp;
        compio_build_zstd_compressor(&comp);
        return comp;
    }

    inline compio_compressor brotli() {
        compio_compressor comp;
        compio_build_brotli_compressor(&comp);
        return comp;
    }
}

} // namespace compio

#endif // COMPIO_HPP

