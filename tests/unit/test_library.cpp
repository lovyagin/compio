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

    void Reset() {
        // close and open file (cursor in the beginning after opening)

        compio_close_file(file);
        compio_close_archive(archive);
        
        archive = compio_open_archive(fn, "r+", &config);
        file = compio_open_file("A", archive);
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
    std::vector<unsigned char> in_data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const std::size_t size = in_data.size();

    std::vector<unsigned char> out_data(size, '?');

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_write(in_data.data(), size, file), size);
    EXPECT_EQ(compio_tell(file), size);
    
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_CUR), 0);
    EXPECT_EQ(compio_tell(file), size);
    
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_END), 0);
    EXPECT_EQ(compio_tell(file), size);

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_read(out_data.data(), size, file), size);

    for (std::size_t i = 0; i < size; ++i) {
        EXPECT_EQ(in_data[i], out_data[i]);
    }
}

std::vector<unsigned char> generate_random_buffer(std::size_t size) {
    std::vector<unsigned char> data(size);

    std::independent_bits_engine<std::default_random_engine, 32, uint32_t> eng;

    for (uint64_t i = 0; i < size; i += sizeof(uint32_t)) {
        auto x = eng();
        for (int j = 0; j < sizeof(uint32_t) && i + j < size; ++j) {
            data[i + j] = reinterpret_cast<char*>(&x)[j];
        }
    }

    return data;
}

TEST_P(WriteReadNBytesTest, RandomWriteRead) {
    uint64_t size = GetParam();
    auto in_data = generate_random_buffer(size);
    std::vector<unsigned char> out_data(size, '?');

    EXPECT_EQ(compio_write(in_data.data(), size, file), size);
    EXPECT_EQ(compio_tell(file), size);

    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_END), 0);
    EXPECT_EQ(compio_tell(file), size);
    
    EXPECT_EQ(compio_seek(file, 0, COMP_SEEK_SET), 0);
    EXPECT_EQ(compio_tell(file), 0);
    EXPECT_EQ(compio_read(out_data.data(), size, file), size);
    EXPECT_EQ(compio_tell(file), size);

    for (std::size_t i = 0; i < size; ++i) {
        EXPECT_EQ(in_data[i], out_data[i]);
    }
}

TEST_P(WriteReadNBytesTest, RandomWriteResetRead) {
    uint64_t size = GetParam();
    auto in_data = generate_random_buffer(size);
    std::vector<unsigned char> out_data(size, '?');

    EXPECT_EQ(compio_write(in_data.data(), size, file), size);

    Reset();

    EXPECT_EQ(compio_read(out_data.data(), size, file), size);

    for (std::size_t i = 0; i < size; ++i) {
        EXPECT_EQ(in_data[i], out_data[i]);
    }
}

INSTANTIATE_TEST_CASE_P(
    WriteReadTests,
    WriteReadNBytesTest,
    ::testing::Values(1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)
);