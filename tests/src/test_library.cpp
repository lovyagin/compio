#include <algorithm>
#include <gtest/gtest.h>
#include <random>

#include "compio/compio_file.hpp"
#include "compio.h"

#include "sample_data.hpp"
#include "test_util.hpp"

class WriteReadNBytesTest : public ::testing::TestWithParam<uint64_t> {
protected:
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];
    bool failed = false;

    void SetUp() override {
        compio_build_default_config(&config);

        generate_tmp_fn(fn, sizeof(fn));

        archive = compio_open_archive(fn, "w+", &config);
        if (!archive) {
            failed = true;
            FAIL() << "failed to open archive";
        }
        file = compio_open_file("A", archive);
        if (!file) {
            failed = true;
            FAIL() << "failed to open file in archive";
        }
    }

    void TearDown() override {
        if (file) {
            ASSERT_EQ(compio_close_file(file), 0);
        }
        if (archive) {
            ASSERT_EQ(compio_close_archive(archive), 0);
        }

        remove(fn);
    }

    void Reset() {
        // close and open file (cursor in the beginning after opening)

        if (file) {
            ASSERT_EQ(compio_close_file(file), 0);
        }
        if (archive) {
            ASSERT_EQ(compio_close_archive(archive), 0);
        }
        archive = compio_open_archive(fn, "r+", &config);
        if (!archive) {
            failed = true;
            FAIL() << "failed to reopen archive";
        }
        file = compio_open_file("A", archive);
        if (!file) {
            failed = true;
            FAIL() << "failed to reopen file in archive";
        }
    }
};

class OpenedFileTest : public ::testing::Test {
protected:
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];
    bool failed = false;

    void SetUp() override {
        compio_build_default_config(&config);

        generate_tmp_fn(fn, sizeof(fn));

        archive = compio_open_archive(fn, "w+", &config);
        if (!archive) {
            failed = true;
            FAIL() << "failed to open archive";
        }
        file = compio_open_file("A", archive);
        if (!file) {
            failed = true;
            FAIL() << "failed to open file in archive";
        }
    }

    void TearDown() override {
        if (file) {
            ASSERT_EQ(compio_close_file(file), 0);
        }
        if (archive) {
            ASSERT_EQ(compio_close_archive(archive), 0);
        }

        remove(fn);
    }
};

TEST_F(OpenedFileTest, OpenClose) { ASSERT_FALSE(failed); }

TEST_F(OpenedFileTest, BasicWriteRead) {
    ASSERT_FALSE(failed);
    std::vector<unsigned char> in_data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const std::size_t size = in_data.size();

    std::vector<unsigned char> out_data(size, '?');

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_write(in_data.data(), size, file), size);
    ASSERT_EQ(compio_tell(file), size);

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_CUR), 0);
    ASSERT_EQ(compio_tell(file), size);

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_END), 0);
    ASSERT_EQ(compio_tell(file), size);

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_read(out_data.data(), size, file), size);

    for (std::size_t i = 0; i < size; ++i) {
        ASSERT_EQ(in_data[i], out_data[i]);
    }
}

std::vector<unsigned char> generate_random_buffer(std::size_t size) {
    std::vector<unsigned char> data(size);

    std::independent_bits_engine<std::default_random_engine, 32, uint32_t> eng;

    for (uint64_t i = 0; i < size; i += sizeof(uint32_t)) {
        auto x = eng();
        for (uint64_t j = 0; j < sizeof(uint32_t) && i + j < size; ++j) {
            data[i + j] = reinterpret_cast<char *>(&x)[j];
        }
    }

    return data;
}

TEST_P(WriteReadNBytesTest, RandomWriteRead) {
    ASSERT_FALSE(failed);
    uint64_t size = GetParam();
    auto in_data = generate_random_buffer(size);
    std::vector<unsigned char> out_data(size, '?');

    ASSERT_EQ(compio_write(in_data.data(), size, file), size);
    ASSERT_EQ(compio_tell(file), size);

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_END), 0);
    ASSERT_EQ(compio_tell(file), size);

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_tell(file), 0);
    ASSERT_EQ(compio_read(out_data.data(), size, file), size);
    ASSERT_EQ(compio_tell(file), size);

    for (std::size_t i = 0; i < size; ++i) {
        ASSERT_EQ(in_data[i], out_data[i]);
    }
}

TEST_P(WriteReadNBytesTest, RandomWriteResetRead) {
    ASSERT_FALSE(failed);
    uint64_t size = GetParam();
    auto in_data = generate_random_buffer(size);
    std::vector<unsigned char> out_data(size, '?');

    ASSERT_EQ(compio_write(in_data.data(), size, file), size);

    Reset();
    ASSERT_FALSE(failed);

    ASSERT_EQ(compio_read(out_data.data(), size, file), size);

    for (std::size_t i = 0; i < size; ++i) {
        ASSERT_EQ(in_data[i], out_data[i]);
    }
}

INSTANTIATE_TEST_CASE_P(WriteReadTests, WriteReadNBytesTest,
                        ::testing::Values(1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096));

class RWBlocksTest : public ::testing::TestWithParam<std::pair<int, int>> {};

TEST_P(RWBlocksTest, ConsecutiveBlocksWriteRead) {
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];

    compio_build_default_config(&config);
    config.block_size = 128;
    config.block_size__minimum = 64;
    config.block_size__maximum = 512;

    generate_tmp_fn(fn, sizeof(fn));

    archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    auto [n_blocks, block_size] = GetParam();

    auto in_data = generate_random_buffer(block_size);
    std::vector<unsigned char> out_data(block_size, '?');

    for (int i = 0; i < n_blocks; ++i) {
        ASSERT_EQ(compio_write(in_data.data(), block_size, file), block_size);
        fflush(archive->file);
    }

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = compio_open_archive(fn, "r+", &config);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    for (int i = 0; i < n_blocks; ++i) {
        ASSERT_EQ(compio_tell(file), block_size * i) << "; iter=" << i;
        ASSERT_EQ(compio_read(out_data.data(), block_size, file), block_size) << "; iter=" << i;

        for (int i = 0; i < block_size; ++i) {
            ASSERT_EQ(in_data[i], out_data[i]);
        }
    }

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    remove(fn);
}

INSTANTIATE_TEST_CASE_P(
    RWBlocksTests, RWBlocksTest,
    ::testing::Values(std::pair<int, int>(2, 4), std::pair<int, int>(16, 16),
                      std::pair<int, int>(32, 16), std::pair<int, int>(64, 16),
                      std::pair<int, int>(4, 128), std::pair<int, int>(8, 128),
                      std::pair<int, int>(16, 128), std::pair<int, int>(64, 128),
                      std::pair<int, int>(256, 128), std::pair<int, int>(16, 203),
                      std::pair<int, int>(64, 203), std::pair<int, int>(256, 203)));

class RandomUsageTest : public ::testing::TestWithParam<std::tuple<int, int, int>> {};

TEST_P(RandomUsageTest, RandomUsage) {
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];

    compio_build_default_config(&config);

    generate_tmp_fn(fn, sizeof(fn));

    auto [max_file_size, n_operations, n_repetitions] = GetParam();

    std::minstd_rand rng;
#ifdef COMPIO_DISABLE_INSERT_ERASE
    std::uniform_int_distribution<int> d_op(0, 3);
#else
    std::uniform_int_distribution<int> d_op(0, 5);
#endif
    std::uniform_int_distribution<int> d_pos(0, max_file_size - 2);

    for (int k = 0; k < n_repetitions; ++k) {
        rng.seed(k);

        std::vector<unsigned char> file_data;
        std::vector<unsigned char> buffer(max_file_size);
        int cursor = 0;

        archive = compio_open_archive(fn, "w+", &config);
        ASSERT_NE(archive, nullptr);
        file = compio_open_file("A", archive);
        ASSERT_NE(file, nullptr);

        for (int i = 0; i < n_operations; ++i) {
            switch (d_op(rng)) {
            case 0: {
                cursor = d_pos(rng);
                ASSERT_EQ(compio_seek(file, cursor, COMPIO_SEEK_SET), 0);
                break;
            }
            case 1: {
                ASSERT_EQ(compio_tell(file), cursor);
                break;
            }
            case 2: {
                if (static_cast<size_t>(cursor) < file_data.size()) {
                    int max_size = static_cast<int>(file_data.size()) - cursor;
                    std::uniform_int_distribution<int> d_size(1, 2 * max_size);
                    int size = d_size(rng);
                    int actual_read_size = std::min(size, max_size);

                    ASSERT_EQ(compio_read(buffer.data(), size, file), actual_read_size);
                    for (int i = 0; i < actual_read_size; ++i) {
                        ASSERT_EQ(buffer[i], file_data[cursor + i]) << "; i=" << i;
                    }
                    cursor += actual_read_size;
                }
                break;
            }
            case 3: {
                if (cursor < max_file_size) {
                    std::uniform_int_distribution<int> d_size(
                        1,
                        std::min(static_cast<uint64_t>(max_file_size - cursor), sizeof(html_data)));
                    int size = d_size(rng);
                    std::uniform_int_distribution<int> d_start(0, sizeof(html_data) - size);
                    int start = d_start(rng);

                    ASSERT_EQ(compio_write(html_data + start, size, file), size);
                    if (static_cast<size_t>(cursor + size) > file_data.size()) {
                        file_data.resize(cursor + size, 0);
                    }
                    std::copy_n(html_data + start, size, file_data.data() + cursor);
                    cursor += size;
                }
                break;
            }
            case 4: {
                if (file_data.size() < static_cast<size_t>(max_file_size)) {
                    std::uniform_int_distribution<int> d_size(
                        1, std::min(static_cast<std::size_t>(max_file_size) - std::max(file_data.size(),
                                                             static_cast<std::size_t>(cursor)),
                                    sizeof(html_data)));
                    int size = d_size(rng);
                    std::uniform_int_distribution<int> d_start(0, sizeof(html_data) - size);
                    int start = d_start(rng);

                    if (static_cast<size_t>(cursor) > file_data.size()) {
                        file_data.resize(cursor, 0);
                    }
                    ASSERT_EQ(compio_insert(html_data + start, size, file), size);
                    file_data.insert(file_data.begin() + cursor, html_data + start,
                                     html_data + start + size);
                    cursor += size;
                }
                break;
            }
            case 5: {
                if (cursor < max_file_size) {
                    int max_size = std::max(0, static_cast<int>(file_data.size()) - cursor);
                    std::uniform_int_distribution<int> d_size(1, max_file_size - cursor);
                    int size = d_size(rng);
                    int actual_erase_size = std::min(size, max_size);

                    ASSERT_EQ(compio_erase(size, file), actual_erase_size)
                        << file_data.size() << " " << cursor << " " << max_size << " " << size
                        << " " << actual_erase_size;
                    if (static_cast<size_t>(cursor) < file_data.size()) {
                        file_data.erase(file_data.begin() + cursor,
                                        file_data.begin() + cursor + actual_erase_size);
                    }
                }
                break;
            }
            }

            ASSERT_EQ(file_data.size(), compio_get_size(file));
            ASSERT_LE(file_data.size(), max_file_size);
        }

        ASSERT_EQ(compio_close_file(file), 0);
        ASSERT_EQ(compio_close_archive(archive), 0);
    }

    remove(fn);
}

INSTANTIATE_TEST_CASE_P(RandomUsageTests, RandomUsageTest,
                        ::testing::Values(std::tuple<int, int, int>(5000, 200, 100),
                                          std::tuple<int, int, int>(5000, 1000, 20),
                                          std::tuple<int, int, int>(10000, 1000, 20),
                                          std::tuple<int, int, int>(10000, 10000, 5),
                                          std::tuple<int, int, int>(50000, 500, 20),
                                          std::tuple<int, int, int>(50000, 1000, 5)));

enum OperationType { WRITE, READ };

struct Operation {
    OperationType type;
    int pos, size;
};

struct UsageParams {
    int file_size;
    std::vector<Operation> operations;
};

class CustomUsageTest : public ::testing::TestWithParam<UsageParams> {};

TEST_P(CustomUsageTest, CustomUsage) {
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];

    compio_build_default_config(&config);
    config.block_size = 16;
    config.block_size__minimum = 0;
    config.block_size__maximum = config.block_size * 4;
    generate_tmp_fn(fn, sizeof(fn));

    std::minstd_rand rng(0);

    auto params = GetParam();

    std::vector<unsigned char> file_data(params.file_size, 0);
    std::vector<unsigned char> buffer(params.file_size);
    int cursor = 0;

    archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    for (const auto &operation : params.operations) {
        // fprintf(stderr, "cursor=%d, current_fsize=%d\n", cursor, current_fsize);
        cursor = operation.pos;
        // fprintf(stderr, "compio_seek(%d)\n", cursor);
        ASSERT_EQ(compio_seek(file, cursor, COMPIO_SEEK_SET), 0);

        switch (operation.type) {
        case OperationType::READ: {
            // fprintf(stderr, "compio_read(%d, %d)\n", cursor, size);
            ASSERT_EQ(compio_read(buffer.data(), operation.size, file), operation.size);
            for (int i = 0; i < operation.size; ++i) {
                ASSERT_EQ(buffer[i], file_data[cursor + i]);
            }
            cursor += operation.size;
            break;
        }
        case OperationType::WRITE: {
            std::uniform_int_distribution<int> d_start(0, sizeof(html_data) - operation.size);
            int start = d_start(rng);
            // fprintf(stderr, "compio_write(%d, %d)\n", start, size);
            ASSERT_EQ(compio_write(html_data + start, operation.size, file), operation.size);
            std::copy_n(html_data + start, operation.size, file_data.data() + cursor);
            cursor += operation.size;
            break;
        }
        }
    }

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    remove(fn);
}

INSTANTIATE_TEST_CASE_P(CustomUsageTests, CustomUsageTest,
                        ::testing::Values(UsageParams{30,
                                                      {
                                                          {OperationType::WRITE, 8, 16},
                                                          {OperationType::READ, 0, 24},

                                                          {OperationType::WRITE, 18, 2},
                                                          {OperationType::READ, 0, 24},

                                                          {OperationType::WRITE, 3, 14},
                                                          {OperationType::READ, 0, 24},

                                                          {OperationType::WRITE, 4, 2},
                                                          {OperationType::READ, 0, 24},

                                                          {OperationType::WRITE, 10, 16},
                                                          {OperationType::READ, 0, 26},
                                                      }}));

class ManySmallWritesOneBigReadTest : public ::testing::TestWithParam<std::tuple<int, int, int>> {};

TEST_P(ManySmallWritesOneBigReadTest, ManySmallWritesOneBigRead) {
    auto [num_writes, write_size, cache_size__blocks] = GetParam();

    compio_config config;
    compio_build_default_config(&config);
    config.cache_size__blocks = cache_size__blocks;

    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    compio_archive *archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);
    compio_file *file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    const uint64_t total_size = num_writes * write_size;

    std::vector<unsigned char> expected_data(total_size);
    for (int i = 0; i < num_writes; ++i) {
        int start_offset = (i * write_size) % (sizeof(html_data) - write_size);
        std::copy_n(html_data + start_offset, write_size, expected_data.data() + i * write_size);
    }

    for (int i = 0; i < num_writes; ++i) {
        int start_offset = (i * write_size) % (sizeof(html_data) - write_size);
        ASSERT_EQ(compio_write(html_data + start_offset, write_size, file), write_size)
            << "Write failed at iteration " << i;
    }

    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), 0);

    std::vector<unsigned char> actual_data(total_size);
    ASSERT_EQ(compio_read(actual_data.data(), total_size, file), total_size) << "Big read failed";

    for (uint64_t i = 0; i < total_size; ++i) {
        ASSERT_EQ(expected_data[i], actual_data[i]) << "Data mismatch at byte " << i;
    }

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    remove(fn);
}

INSTANTIATE_TEST_CASE_P(ManySmallWritesOneBigReadTests, ManySmallWritesOneBigReadTest,
                        ::testing::Combine(::testing::Values(10, 100, 1000),
                                           ::testing::Values(1, 4, 16, 256),
                                           ::testing::Values(0, 10, 100)));
