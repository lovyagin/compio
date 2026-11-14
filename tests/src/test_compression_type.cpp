#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "compio.h"

#include "test_util.hpp"

TEST(CompressionTypeTest, ExistingArchive) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    // create archive with some specific compression_type
    compio_config config;
    compio_build_default_config(&config);

    compio_build_zlib_compressor(&config.compressor);
    compio_compression_type type = config.compressor.compression_type;

    auto archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);

    auto file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    // check, that get_compression_type returns same compression_type
    compio_compression_type new_type;
    compio_get_compression_type(fn, &new_type);

    ASSERT_EQ(type, new_type);

    // check, that opening this archive with correct compression type is fine
    archive = compio_open_archive(fn, "r", &config);
    ASSERT_NE(archive, nullptr);
    ASSERT_EQ(compio_close_archive(archive), 0);

    // check, that opening this archive with another compression type will lead to an error
    compio_build_lz4_compressor(&config.compressor);
    ASSERT_EQ(compio_open_archive(fn, "r", &config), nullptr);

    remove(fn);
}

namespace {

int xor_code(const compio_compressor *comp, void *dst, uint64_t *dst_size, const void *src,
             uint64_t src_size) {
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

compio_config xor_config(int id_offset, uint8_t key) {
    compio_config config;
    compio_build_default_config(&config);
    config.compressor.compress = xor_code;
    config.compressor.decompress = xor_code;
    config.compressor.get_bufsize = xor_bufsize;
    config.compressor.compression_type = static_cast<compio_compression_type>(COMPIO_COMPRESS_CUSTOM + id_offset);
    config.compressor.level = key;
    return config;
}

const char payload[] = "external codec payload";

void write_payload(const char *fn, const compio_config &config) {
    auto archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);
    auto file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(compio_write(payload, sizeof(payload), file), sizeof(payload));
    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);
}

void remove_archive(const char *fn) {
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}

} // namespace

TEST(CompressionTypeTest, CustomCompressorIdentityIsChecked) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    write_payload(fn, xor_config(1, 0x55));

    const compio_config other = xor_config(2, 0xAA);
    errno = 0;
    EXPECT_EQ(compio_open_archive(fn, "r", &other), nullptr);
    EXPECT_EQ(errno, EINVAL);

    const compio_config same = xor_config(1, 0x55);
    auto archive = compio_open_archive(fn, "r", &same);
    ASSERT_NE(archive, nullptr);
    auto file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);
    char buf[sizeof(payload)] = {};
    ASSERT_EQ(compio_read(buf, sizeof(buf), file), sizeof(buf));
    EXPECT_EQ(memcmp(buf, payload, sizeof(payload)), 0);
    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    compio_compression_type stored;
    ASSERT_EQ(compio_get_compression_type(fn, &stored), 0);
    EXPECT_EQ(stored, same.compressor.compression_type);

    remove_archive(fn);
}

TEST(CompressionTypeTest, InvalidCompressorIsRejected) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    for (int type : {0, -1, COMPIO_COMPRESS_BROTLI + 1, COMPIO_COMPRESS_CUSTOM - 1}) {
        compio_config config = xor_config(0, 0);
        config.compressor.compression_type = static_cast<compio_compression_type>(type);
        errno = 0;
        EXPECT_EQ(compio_open_archive(fn, "w+", &config), nullptr) << type;
        EXPECT_EQ(errno, EINVAL) << type;
    }

    compio_config config = xor_config(0, 0);
    config.compressor.decompress = nullptr;
    errno = 0;
    EXPECT_EQ(compio_open_archive(fn, "w+", &config), nullptr);
    EXPECT_EQ(errno, EINVAL);

    remove_archive(fn);
}

TEST(CompressionTypeTest, BuildByTypeRefusesCustom) {
    compio_compressor compressor;
    for (int type = COMPIO_COMPRESS_DUMMY; type <= COMPIO_COMPRESS_BROTLI; ++type) {
        ASSERT_EQ(compio_build_compressor_by_type(&compressor, static_cast<compio_compression_type>(type)),
                  COMPIO_SUCCESS);
        EXPECT_EQ(compressor.compression_type, type);
    }
    errno = 0;
    EXPECT_EQ(compio_build_compressor_by_type(&compressor, COMPIO_COMPRESS_CUSTOM), COMPIO_ERROR);
    EXPECT_EQ(errno, ENOTSUP);
}

TEST(CompressionTypeTest, RepairRefusesCustomCompressor) {
    char fn[256];
    char out[256];
    generate_tmp_fn(fn, sizeof(fn));
    generate_tmp_fn(out, sizeof(out));
    remove(out);
    write_payload(fn, xor_config(1, 0x55));

    EXPECT_EQ(compio_repair(fn, out), COMPIO_ERROR);

    std::filesystem::remove_all(out);
    remove_archive(fn);
}
