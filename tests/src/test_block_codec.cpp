#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

namespace {

int xor_code(const compio_compressor *comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    if (*dst_size < src_size) {
        errno = ENOBUFS;
        return -1;
    }
    const auto *in = static_cast<const uint8_t *>(src);
    auto *out = static_cast<uint8_t *>(dst);
    for (uint64_t i = 0; i < src_size; ++i) out[i] = in[i] ^ static_cast<uint8_t>(comp->level);
    *dst_size = src_size;
    return 0;
}

uint64_t xor_bufsize(const compio_compressor *, uint64_t src_size) { return src_size; }

compio_compressor xor_compressor(int id_offset, uint8_t key) {
    compio_compressor c{};
    c.compress = xor_code;
    c.decompress = xor_code;
    c.get_bufsize = xor_bufsize;
    c.compression_type = static_cast<compio_compression_type>(COMPIO_COMPRESS_CUSTOM + id_offset);
    c.level = key;
    return c;
}

std::vector<uint8_t> pattern(size_t size, unsigned seed) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>((i / 7) * 31 + seed);
    return data;
}

class BlockCodecTest : public ::testing::Test {
protected:
    char fn[256];
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.block_size__minimum = 1024;
        config.block_size__maximum = 8192;
        config.cache_size__blocks = 4;
        compio_build_lz4_compressor(&config.compressor);
    }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    compio_archive *open(const char *mode, const compio_config &c) { return compio_open_archive(fn, mode, &c); }

    static void write_file(compio_archive *archive, const char *name, const std::vector<uint8_t> &data) {
        compio_file *file = compio_open_file(name, archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), file), static_cast<int64_t>(data.size()));
        ASSERT_EQ(compio_close_file(file), 0);
    }

    static bool read_matches(compio_archive *archive, const char *name, const std::vector<uint8_t> &data) {
        compio_file *file = compio_open_file(name, archive);
        if (!file) return false;
        std::vector<uint8_t> back(data.size());
        const int64_t got = compio_read(back.data(), back.size(), file);
        compio_close_file(file);
        return got == static_cast<int64_t>(data.size()) && back == data;
    }

    // Codec of every block stored in the archive.
    static std::multiset<uint32_t> codecs(compio_archive *archive) {
        std::multiset<uint32_t> result;
        compio::tree_key max{UINT64_MAX, UINT64_MAX};
        const auto blocks = archive->index->get_range({0, 0}, max);
        EXPECT_TRUE(blocks.has_value());
        if (!blocks) return result;
        for (const auto &entry : *blocks) {
            compio::storage_block stored;
            EXPECT_TRUE(stored.read_from(archive->file, entry.second.addr));
            result.insert(stored.codec);
        }
        return result;
    }
};

TEST_F(BlockCodecTest, BlocksKeepTheirCodecAfterSwitch) {
    const auto before = pattern(40000, 1);
    const auto after = pattern(40000, 2);

    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "before", before);
    compio_flush(archive);

    compio_compressor zstd;
    compio_build_zstd_compressor(&zstd);
    ASSERT_EQ(compio_set_compressor(archive, &zstd), COMPIO_SUCCESS);
    write_file(archive, "after", after);
    compio_flush(archive);

    const auto kinds = codecs(archive);
    EXPECT_GT(kinds.count(COMPIO_COMPRESS_LZ4), 0u);
    EXPECT_GT(kinds.count(COMPIO_COMPRESS_ZSTD), 0u);
    EXPECT_TRUE(read_matches(archive, "before", before));
    EXPECT_TRUE(read_matches(archive, "after", after));
    ASSERT_EQ(compio_close_archive(archive), 0);

    // The header names the compressor of new blocks; blocks of the other built-in codec still read.
    compio_config reopened = config;
    reopened.compressor = zstd;
    archive = open("r", reopened);
    ASSERT_NE(archive, nullptr);
    EXPECT_TRUE(read_matches(archive, "before", before));
    EXPECT_TRUE(read_matches(archive, "after", after));
    ASSERT_EQ(compio_close_archive(archive), 0);

    compio_compression_type recorded;
    ASSERT_EQ(compio_get_compression_type(fn, &recorded), 0);
    EXPECT_EQ(recorded, COMPIO_COMPRESS_ZSTD);
}

TEST_F(BlockCodecTest, RecompressRewritesOnlyBlocksOfOtherCodecs) {
    const auto data = pattern(60000, 3);
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", data);
    compio_flush(archive);

    compio_compressor zlib;
    compio_build_zlib_compressor(&zlib);
    ASSERT_EQ(compio_set_compressor(archive, &zlib), COMPIO_SUCCESS);

    uint64_t rewritten = 0;
    ASSERT_EQ(compio_recompress(archive, &rewritten), COMPIO_SUCCESS);
    EXPECT_GT(rewritten, 0u);
    const auto kinds = codecs(archive);
    EXPECT_EQ(kinds.count(COMPIO_COMPRESS_LZ4), 0u);
    EXPECT_GT(kinds.count(COMPIO_COMPRESS_ZLIB), 0u);
    EXPECT_TRUE(read_matches(archive, "f", data));

    ASSERT_EQ(compio_recompress(archive, &rewritten), COMPIO_SUCCESS);
    EXPECT_EQ(rewritten, 0u);
    ASSERT_EQ(compio_close_archive(archive), 0);

    compio_config reopened = config;
    reopened.compressor = zlib;
    archive = open("r", reopened);
    ASSERT_NE(archive, nullptr);
    EXPECT_TRUE(read_matches(archive, "f", data));
    ASSERT_EQ(compio_close_archive(archive), 0);
}

TEST_F(BlockCodecTest, RecompressKeepsOpenFilesUsable) {
    const auto data = pattern(30000, 4);
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", data);

    compio_file *file = compio_open_file("f", archive);
    ASSERT_NE(file, nullptr);
    std::vector<uint8_t> back(data.size());
    ASSERT_EQ(compio_read(back.data(), back.size(), file), static_cast<int64_t>(back.size()));

    compio_compressor zstd;
    compio_build_zstd_compressor(&zstd);
    ASSERT_EQ(compio_set_compressor(archive, &zstd), COMPIO_SUCCESS);
    ASSERT_EQ(compio_recompress(archive, nullptr), COMPIO_SUCCESS);

    ASSERT_EQ(compio_seek(file, 0, SEEK_SET), 0);
    std::fill(back.begin(), back.end(), 0);
    ASSERT_EQ(compio_read(back.data(), back.size(), file), static_cast<int64_t>(back.size()));
    EXPECT_EQ(back, data);
    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);
}

TEST_F(BlockCodecTest, ExternalCodecNeedsRegistration) {
    const auto data = pattern(10000, 5);
    config.compressor = xor_compressor(1, 0x55);
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", data);

    compio_compressor lz4;
    compio_build_lz4_compressor(&lz4);
    ASSERT_EQ(compio_set_compressor(archive, &lz4), COMPIO_SUCCESS);
    ASSERT_EQ(compio_close_archive(archive), 0);

    compio_config reopened = config;
    reopened.compressor = lz4;
    archive = open("r+", reopened);
    ASSERT_NE(archive, nullptr);
    errno = 0;
    EXPECT_FALSE(read_matches(archive, "f", data));

    const compio_compressor external = xor_compressor(1, 0x55);
    ASSERT_EQ(compio_register_compressor(archive, &external), COMPIO_SUCCESS);
    EXPECT_TRUE(read_matches(archive, "f", data));
    ASSERT_EQ(compio_close_archive(archive), 0);
}

TEST_F(BlockCodecTest, WrongExternalCompressorIsCaughtByDataChecksum) {
    const auto data = pattern(10000, 6);
    config.compressor = xor_compressor(1, 0x55);
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", data);
    ASSERT_EQ(compio_close_archive(archive), 0);

    // Same identifier, different behaviour: the stored bytes are intact but decode to garbage.
    config.compressor = xor_compressor(1, 0x56);
    archive = open("r", config);
    ASSERT_NE(archive, nullptr);
    EXPECT_FALSE(read_matches(archive, "f", data));
    ASSERT_EQ(compio_close_archive(archive), 0);
}

TEST_F(BlockCodecTest, InvalidCompressorIsRejected) {
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    compio_compressor bad = xor_compressor(1, 0);
    bad.decompress = nullptr;
    errno = 0;
    EXPECT_EQ(compio_set_compressor(archive, &bad), COMPIO_ERROR);
    EXPECT_EQ(errno, EINVAL);
    bad = xor_compressor(1, 0);
    bad.compression_type = static_cast<compio_compression_type>(COMPIO_COMPRESS_CUSTOM - 1);
    errno = 0;
    EXPECT_EQ(compio_set_compressor(archive, &bad), COMPIO_ERROR);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(compio_set_compressor(archive, nullptr), COMPIO_ERROR);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = open("r", config);
    ASSERT_NE(archive, nullptr);
    compio_compressor zstd;
    compio_build_zstd_compressor(&zstd);
    errno = 0;
    EXPECT_EQ(compio_set_compressor(archive, &zstd), COMPIO_ERROR);
    EXPECT_EQ(errno, EROFS);
    EXPECT_EQ(compio_recompress(archive, nullptr), COMPIO_ERROR);
    ASSERT_EQ(compio_close_archive(archive), 0);
}

class IndexNodeChecksumTest : public BlockCodecTest {};

TEST_F(IndexNodeChecksumTest, CorruptedNodeIsNotReadAsData) {
    const auto data = pattern(20000, 7);
    compio_archive *archive = open("w+", config);
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", data);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = open("r", config);
    ASSERT_NE(archive, nullptr);
    const uint64_t root = readonly(archive->header, compio::header)->index_root;
    ASSERT_EQ(compio_close_archive(archive), 0);
    ASSERT_NE(root, 0u);

    // Flip a bit in the middle of the first key.
    FILE *f = fopen(fn, "rb+");
    ASSERT_NE(f, nullptr);
    const uint64_t offset = root + 6 + 3;
    uint8_t byte = 0;
    ASSERT_EQ(compio::fseek64(f, offset, SEEK_SET), 0);
    ASSERT_EQ(fread(&byte, 1, 1, f), 1u);
    byte ^= 0x01;
    ASSERT_EQ(compio::fseek64(f, offset, SEEK_SET), 0);
    ASSERT_EQ(fwrite(&byte, 1, 1, f), 1u);
    fclose(f);

    archive = open("r", config);
    const bool intact = archive != nullptr && read_matches(archive, "f", data);
    if (archive) compio_close_archive(archive);
    EXPECT_FALSE(intact);
}

} // namespace
