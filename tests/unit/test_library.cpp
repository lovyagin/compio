#include <gtest/gtest.h>
#include <random>

#include "compio.h"

class WriteReadNBytesTest : public ::testing::TestWithParam<uint64_t> {
protected:
    compio_config config;
    compio_archive* archive;
    compio_file* file;
    char fn[L_tmpnam];

    void SetUp() override {
        compio_build_default_config(&config);

        tmpnam(fn);

        archive = compio_open_archive(fn, "w+", &config);
        file = compio_open_file("A", archive);
    }

    void TearDown() override {
        compio_close_file(file);
        compio_close_archive(archive);

        remove(fn);
    }
};

class OpenedFileTest : public ::testing::Test {
protected:
    compio_config config;
    compio_archive* archive;
    compio_file* file;
    char fn[L_tmpnam];

    void SetUp() override {
        compio_build_default_config(&config);

        tmpnam(fn);

        archive = compio_open_archive(fn, "w+", &config);
        file = compio_open_file("A", archive);
    }

    void TearDown() override {
        compio_close_file(file);
        compio_close_archive(archive);

        remove(fn);
    }
};

TEST_F(OpenedFileTest, OpenClose) {
    EXPECT_NE(archive, nullptr);
    EXPECT_NE(file, nullptr);
}

TEST_F(OpenedFileTest, BasicWriteRead) {
    unsigned char in_data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    unsigned char out_data[sizeof(in_data)];
    std::fill_n(out_data, sizeof(in_data), '?');

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_write(in_data, sizeof(in_data), file), sizeof(in_data));
    EXPECT_EQ(compio_tell(file), sizeof(in_data));
    
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_CUR), 0);
    EXPECT_EQ(compio_tell(file), sizeof(in_data));
    
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_END), 0);
    EXPECT_EQ(compio_tell(file), sizeof(in_data));

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_read(out_data, sizeof(in_data), file), sizeof(in_data));

    for (uint64_t i = 0; i < sizeof(in_data); ++i) {
        EXPECT_EQ(in_data[i], out_data[i]);
    }
}

TEST_P(WriteReadNBytesTest, RandomWriteRead) {
    uint64_t size = GetParam();
    unsigned char* in_data = new unsigned char[size];
    unsigned char* out_data = new unsigned char[size];
    std::fill_n(out_data, size, '?');

    std::independent_bits_engine<std::default_random_engine, 32, uint32_t> eng;

    // randomly fill in_data with bytes
    for (uint64_t i = 0; i < size; i += sizeof(uint32_t)) {
        auto x = eng();
        for (int j = 0; j < sizeof(uint32_t) && i + j < size; ++j) {
            in_data[i + j] = reinterpret_cast<char*>(&x)[j];
        }
    }

    // write and read
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_write(in_data, size, file), size);
    EXPECT_EQ(compio_tell(file), size);

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_END), 0);
    EXPECT_EQ(compio_tell(file), size);

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_read(out_data, size, file), size);

    // check in_data == out_data
    for (uint64_t i = 0; i < size; ++i) {
        EXPECT_EQ(in_data[i], out_data[i]);
    }
}

INSTANTIATE_TEST_CASE_P(
    WriteReadTests,
    WriteReadNBytesTest,
    ::testing::Values(1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)
);