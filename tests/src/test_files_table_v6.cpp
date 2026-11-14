#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

using compio::files_table;

namespace {

class FilesTableV6Test : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    compio_archive *open(const char *mode) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        return compio_open_archive(fn, mode, &cfg);
    }

    void write_file(compio_archive *archive, const std::string &name, const std::string &data) {
        compio_file *f = compio_open_file(name.c_str(), archive);
        ASSERT_NE(f, nullptr) << name;
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        ASSERT_EQ(compio_close_file(f), COMPIO_SUCCESS);
    }

    std::string read_file(compio_archive *archive, const std::string &name) {
        compio_file *f = compio_open_file(name.c_str(), archive);
        if (!f) return "<missing>";
        std::string data(compio_get_size(f), '\0');
        const uint64_t n = compio_read(data.data(), data.size(), f);
        compio_close_file(f);
        data.resize(n);
        return data;
    }

    void patch(uint64_t offset, uint8_t value) {
        FILE *f = fopen(fn, "r+b");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fseek(f, static_cast<long>(offset), SEEK_SET), 0);
        ASSERT_EQ(fputc(value, f), value);
        fclose(f);
    }
};

std::vector<uint8_t> record(const std::string &name, uint64_t size, uint64_t id) {
    files_table table;
    files_table::file *f = table.add(name.c_str());
    f->size = size;
    f->file_id = id;
    return table.serialize();
}

} // namespace

TEST_F(FilesTableV6Test, LongNamesAndPathsSurviveReopen) {
    const std::vector<std::string> names = {
        "a",
        "run3/gen_12/points.bin",
        "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd1\x80.txt",
        std::string(COMPIO_FNAME_MAX_SIZE - 1, 'n'),
    };
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    for (const auto &name : names) write_file(archive, name, "data of " + name.substr(0, 8));
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    for (const auto &name : names) EXPECT_EQ(read_file(archive, name), "data of " + name.substr(0, 8));
    // The long name packs well, so the table is stored compressed.
    const auto &h = *readonly(archive->header, compio::header);
    EXPECT_EQ(h.files_table_encoding, static_cast<uint32_t>(COMPIO_COMPRESS_LZ4));
    EXPECT_LT(h.files_table_size, h.ftable.serialized_size());
    compio_close_archive(archive);
}

TEST_F(FilesTableV6Test, EmptyAndOverlongNamesAreRejected) {
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    errno = 0;
    EXPECT_EQ(compio_open_file("", archive), nullptr);
    EXPECT_EQ(errno, EINVAL);
    const std::string too_long(COMPIO_FNAME_MAX_SIZE, 'x');
    errno = 0;
    EXPECT_EQ(compio_open_file(too_long.c_str(), archive), nullptr);
    EXPECT_EQ(errno, ENAMETOOLONG);
    compio_close_archive(archive);
}

TEST_F(FilesTableV6Test, RemovedFilesLeaveNoRecord) {
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    write_file(archive, "keep", "k");
    write_file(archive, "drop", "d");
    ASSERT_EQ(compio_remove_file(archive, "drop"), 0);
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    const auto &h = *readonly(archive->header, compio::header);
    ASSERT_EQ(h.ftable.files.size(), 1u);
    EXPECT_EQ(h.files_table_size, files_table::record_header_size + 4);
    EXPECT_EQ(read_file(archive, "keep"), "k");
    compio_close_archive(archive);
}

TEST_F(FilesTableV6Test, CorruptTableIsDetected) {
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    write_file(archive, "file", "content");
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = open("r");
    ASSERT_NE(archive, nullptr);
    const uint64_t table_addr = readonly(archive->header, compio::header)->files_table_addr;
    compio_close_archive(archive);
    ASSERT_NE(table_addr, 0u);

    // Flip a byte of the stored name.
    patch(table_addr + files_table::record_header_size, 'F');
    EXPECT_EQ(open("r"), nullptr);
}

TEST_F(FilesTableV6Test, PreviousFormatIsRejected) {
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);

    // Both header slots start with the magic number; 27110663 was format v6.
    const uint32_t v6 = 27110663;
    for (uint64_t slot : {uint64_t{0}, compio::header::disk_size_bytes}) {
        for (int i = 0; i < 4; ++i) patch(slot + i, static_cast<uint8_t>(v6 >> (8 * i)));
    }
    EXPECT_EQ(open("r"), nullptr);
}

TEST(FilesTableParseTest, RoundTrip) {
    files_table table;
    table.add("x")->size = 7;
    table.add("dir/y")->size = 9;
    const std::vector<uint8_t> bytes = table.serialize();
    ASSERT_EQ(bytes.size(), table.serialized_size());

    files_table parsed;
    ASSERT_TRUE(parsed.parse(bytes.data(), bytes.size(), 2));
    ASSERT_NE(parsed.find("dir/y"), nullptr);
    EXPECT_EQ(parsed.find("dir/y")->size, 9u);
    EXPECT_EQ(parsed.find("x")->file_id, table.find("x")->file_id);
    // New ids continue after the largest one read back.
    EXPECT_GT(parsed.add("z")->file_id, table.find("dir/y")->file_id);
}

TEST(FilesTableParseTest, RejectsMalformedRecords) {
    files_table parsed;
    std::vector<uint8_t> bytes = record("name", 1, 1);

    EXPECT_FALSE(parsed.parse(bytes.data(), bytes.size(), 2)) << "count mismatch";
    EXPECT_FALSE(parsed.parse(bytes.data(), bytes.size() - 1, 1)) << "truncated name";
    EXPECT_FALSE(parsed.parse(bytes.data(), files_table::record_header_size - 1, 1)) << "truncated header";

    std::vector<uint8_t> empty_name = bytes;
    empty_name[0] = empty_name[1] = 0;
    EXPECT_FALSE(parsed.parse(empty_name.data(), files_table::record_header_size, 1));

    std::vector<uint8_t> embedded_nul = bytes;
    embedded_nul[files_table::record_header_size + 1] = 0;
    EXPECT_FALSE(parsed.parse(embedded_nul.data(), embedded_nul.size(), 1));

    std::vector<uint8_t> duplicate_name = bytes;
    const std::vector<uint8_t> other_id = record("name", 1, 2);
    duplicate_name.insert(duplicate_name.end(), other_id.begin(), other_id.end());
    EXPECT_FALSE(parsed.parse(duplicate_name.data(), duplicate_name.size(), 2));

    std::vector<uint8_t> duplicate_id = bytes;
    const std::vector<uint8_t> same_id = record("other", 1, 1);
    duplicate_id.insert(duplicate_id.end(), same_id.begin(), same_id.end());
    EXPECT_FALSE(parsed.parse(duplicate_id.data(), duplicate_id.size(), 2));

    EXPECT_TRUE(parsed.parse(bytes.data(), bytes.size(), 1));
}
