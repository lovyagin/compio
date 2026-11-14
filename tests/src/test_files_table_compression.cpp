#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "test_util.hpp"

namespace {

using compio::files_table;

class FilesTableCompressionTest : public ::testing::Test {
protected:
    char fn[256];
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        compio_build_default_config(&config);
    }

    compio_archive *open(const char *mode) { return compio_open_archive(fn, mode, &config); }

    // Names with a long common prefix, as an application that stores a tree of results makes them.
    static std::string name_of(int i) { return "results/run_0042/generation_" + std::to_string(i) + "/points.bin"; }

    void write_files(int count) {
        compio_archive *archive = open("w+");
        ASSERT_NE(archive, nullptr);
        for (int i = 0; i < count; ++i) {
            compio_file *file = compio_open_file(name_of(i).c_str(), archive);
            ASSERT_NE(file, nullptr);
            const std::string data = std::to_string(i);
            ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
            compio_close_file(file);
        }
        ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    }

    void patch(uint64_t offset, uint8_t byte) {
        FILE *f = fopen(fn, "r+b");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fseek(f, static_cast<long>(offset), SEEK_SET), 0);
        uint8_t old = 0;
        ASSERT_EQ(fread(&old, 1, 1, f), 1u);
        ASSERT_EQ(fseek(f, static_cast<long>(offset), SEEK_SET), 0);
        byte = static_cast<uint8_t>(old ^ byte);
        ASSERT_EQ(fwrite(&byte, 1, 1, f), 1u);
        fclose(f);
    }
};

TEST_F(FilesTableCompressionTest, TableIsPackedWithTheArchiveCompressor) {
    compio_build_zstd_compressor(&config.compressor);
    write_files(2000);

    compio_archive *archive = open("r");
    ASSERT_NE(archive, nullptr);
    const auto &h = *readonly(archive->header, compio::header);
    EXPECT_EQ(h.files_table_encoding, static_cast<uint32_t>(COMPIO_COMPRESS_ZSTD));
    EXPECT_LT(h.files_table_size * 4, h.ftable.serialized_size());
    for (int i = 0; i < 2000; i += 97) {
        compio_file *file = compio_open_file(name_of(i).c_str(), archive);
        ASSERT_NE(file, nullptr) << name_of(i);
        std::string back(compio_get_size(file), '\0');
        ASSERT_EQ(compio_read(back.data(), back.size(), file), back.size());
        EXPECT_EQ(back, std::to_string(i));
        compio_close_file(file);
    }
    compio_close_archive(archive);
}

TEST_F(FilesTableCompressionTest, CorruptPackedTableIsDetected) {
    write_files(500);
    compio_archive *archive = open("r");
    ASSERT_NE(archive, nullptr);
    const auto &h = *readonly(archive->header, compio::header);
    ASSERT_NE(h.files_table_encoding, 0u);
    const uint64_t addr = h.files_table_addr;
    const uint64_t size = h.files_table_size;
    compio_close_archive(archive);

    patch(addr + size / 2, 0x40);
    EXPECT_EQ(open("r"), nullptr);
}

TEST_F(FilesTableCompressionTest, UnchangedTableIsNotRewritten) {
    write_files(300);
    compio_archive *archive = open("r+");
    ASSERT_NE(archive, nullptr);
    const uint64_t addr = readonly(archive->header, compio::header)->files_table_addr;
    compio_flush(archive);
    EXPECT_EQ(readonly(archive->header, compio::header)->files_table_addr, addr);
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
}

TEST_F(FilesTableCompressionTest, ExternalAndDummyCompressorsLeaveTheTablePlain) {
    compio_build_dummy_compressor(&config.compressor);
    write_files(300);
    compio_archive *archive = open("r");
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(readonly(archive->header, compio::header)->files_table_encoding, 0u);
    compio_close_archive(archive);

    // An external compressor may be missing at the next open, so the table never depends on it.
    std::vector<uint8_t> plain(4000, 'a');
    uint32_t encoding = 1;
    const auto stored = files_table::encode(plain, COMPIO_COMPRESS_CUSTOM + 1, encoding);
    EXPECT_EQ(encoding, 0u);
    EXPECT_EQ(stored, plain);
}

TEST(FilesTableEncodeTest, TableThatDoesNotShrinkStaysPlain) {
    files_table table;
    table.add("x");
    const auto plain = table.serialize();
    uint32_t encoding = 1;
    EXPECT_EQ(files_table::encode(plain, COMPIO_COMPRESS_ZSTD, encoding), plain);
    EXPECT_EQ(encoding, 0u);
}

} // namespace
