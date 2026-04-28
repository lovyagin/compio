#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

#include "compio/btree.hpp"
#include "compio/compio_file.hpp"
#include "compio.h"

#include "sample_data.hpp"
#include "test_util.hpp"

class InsertEraseTest : public ::testing::TestWithParam<int> {
protected:
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];
    bool failed = false;

    void SetUp() override {
        compio_build_default_config(&config);
        config.cache_size__blocks = GetParam();

        generate_tmp_fn(fn, sizeof(fn));

        archive = compio_open_archive(fn, "w+", &config);
        if (!archive) {
            failed = true;
            FAIL() << "failed to open archive";
        }
        file = compio_open_file("test_file", archive);
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

        remove(fn); remove((std::string(fn) + ".wal").c_str());
    }

    // Helper function to verify file content
    void VerifyFileContent(const std::vector<unsigned char> &expected) {
        uint64_t file_size = compio_get_size(file);
        ASSERT_EQ(file_size, expected.size()) << "File size mismatch";

        if (expected.empty())
            return;

        std::vector<unsigned char> actual(file_size);
        compio_seek(file, 0, COMPIO_SEEK_SET);
        uint64_t bytes_read = compio_read(actual.data(), file_size, file);
        ASSERT_EQ(bytes_read, file_size) << "Failed to read entire file";

        ASSERT_EQ(actual, expected) << "File content mismatch";
    }

    // Helper function to write initial data
    void WriteInitialData(const std::vector<unsigned char> &data) {
        compio_seek(file, 0, COMPIO_SEEK_SET);
        uint64_t bytes_written = compio_write(data.data(), data.size(), file);
        ASSERT_EQ(bytes_written, data.size()) << "Failed to write initial data";
    }

    std::size_t GetBlockCount() {
        const compio::tree_key min_key{file->hash, 0};
        const compio::tree_key max_key{file->hash, UINT64_MAX};
        auto range_opt = archive->index->get_range(min_key, max_key);
        if (!range_opt.has_value()) {
            return 0;
        }
        return range_opt->size();
    }
};

class InsertEraseParamTest : public ::testing::TestWithParam<std::tuple<size_t, size_t, size_t>> {
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
        file = compio_open_file("test_file", archive);
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

        remove(fn); remove((std::string(fn) + ".wal").c_str());
    }

    // Helper function to verify file content
    void VerifyFileContent(const std::vector<unsigned char> &expected) {
        uint64_t file_size = compio_get_size(file);
        ASSERT_EQ(file_size, expected.size()) << "File size mismatch";

        if (expected.empty())
            return;

        std::vector<unsigned char> actual(file_size);
        compio_seek(file, 0, COMPIO_SEEK_SET);
        uint64_t bytes_read = compio_read(actual.data(), file_size, file);
        ASSERT_EQ(bytes_read, file_size) << "Failed to read entire file";

        ASSERT_EQ(actual, expected) << "File content mismatch";
    }

    // Helper function to write initial data
    void WriteInitialData(const std::vector<unsigned char> &data) {
        compio_seek(file, 0, COMPIO_SEEK_SET);
        uint64_t bytes_written = compio_write(data.data(), data.size(), file);
        ASSERT_EQ(bytes_written, data.size()) << "Failed to write initial data";
    }
};

class FragmentationReuseTest : public ::testing::Test {
protected:
    compio_config config;
    compio_archive *archive{};
    compio_file *file{};
    char fn[256]{};

    void SetUp() override {
        compio_build_default_config(&config);
        config.block_size = 4;
        config.block_size__minimum = 2;
        config.block_size__maximum = 8;

        generate_tmp_fn(fn, sizeof(fn));
        archive = compio_open_archive(fn, "w+", &config);
        ASSERT_NE(archive, nullptr);
        file = compio_open_file("frag_file", archive);
        ASSERT_NE(file, nullptr);
    }

    void TearDown() override {
        if (file) {
            ASSERT_EQ(compio_close_file(file), 0);
        }
        if (archive) {
            ASSERT_EQ(compio_close_archive(archive), 0);
        }
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    std::size_t GetBlockCount() {
        const compio::tree_key min_key{file->hash, 0};
        const compio::tree_key max_key{file->hash, UINT64_MAX};
        auto range_opt = archive->index->get_range(min_key, max_key);
        if (!range_opt.has_value()) {
            return 0;
        }
        return range_opt->size();
    }

    void VerifyFileContent(const std::vector<unsigned char> &expected) {
        ASSERT_EQ(compio_get_size(file), expected.size());
        std::vector<unsigned char> actual(expected.size());
        compio_seek(file, 0, COMPIO_SEEK_SET);
        ASSERT_EQ(compio_read(actual.data(), actual.size(), file), actual.size());
        ASSERT_EQ(actual, expected);
    }
};

// Basic Insert Tests
TEST_P(InsertEraseTest, InsertAtBeginning) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 2);
    std::vector<unsigned char> expected = {9, 8, 1, 2, 3, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, InsertAtMiddle) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 2, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 2);
    std::vector<unsigned char> expected = {1, 2, 9, 8, 3, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, InsertAtEnd) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 3, COMPIO_SEEK_SET); // Seek to end
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 2);
    std::vector<unsigned char> expected = {1, 2, 3, 9, 8};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, InsertIntoEmptyFile) {
    // File is already empty

    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 2);
    std::vector<unsigned char> expected = {9, 8};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, InsertBeyondFileEnd) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 7, COMPIO_SEEK_SET); // Seek to position 7 (4 bytes beyond end)
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 2);
    std::vector<unsigned char> expected = {1, 2, 3, 0, 0, 0, 0, 9, 8};
    VerifyFileContent(expected);
}

// Basic Erase Tests
TEST_P(InsertEraseTest, EraseFromBeginning) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 0, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(2, file);

    ASSERT_EQ(erased, 2);
    std::vector<unsigned char> expected = {3, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, EraseFromMiddle) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(2, file);

    ASSERT_EQ(erased, 2);
    std::vector<unsigned char> expected = {1, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, EraseFromEnd) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 3, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(2, file);

    ASSERT_EQ(erased, 2);
    std::vector<unsigned char> expected = {1, 2, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, EraseEntireFile) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 0, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(5, file);

    ASSERT_EQ(erased, 5);
    std::vector<unsigned char> expected = {};
    VerifyFileContent(expected);
}

// Edge Cases
TEST_P(InsertEraseTest, ZeroSizeInsert) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t inserted = compio_insert(nullptr, 0, file);

    ASSERT_EQ(inserted, 0);
    std::vector<unsigned char> expected = {1, 2, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, ZeroSizeErase) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(0, file);

    ASSERT_EQ(erased, 0);
    std::vector<unsigned char> expected = {1, 2, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, SingleByteInsert) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    unsigned char single_byte = 9;
    uint64_t inserted = compio_insert(&single_byte, 1, file);

    ASSERT_EQ(inserted, 1);
    std::vector<unsigned char> expected = {1, 9, 2, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, SingleByteErase) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(1, file);

    ASSERT_EQ(erased, 1);
    std::vector<unsigned char> expected = {1, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, EraseBeyondFileSize) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 2, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(5, file); // Try to erase 5 bytes from position 2

    ASSERT_EQ(erased, 1); // Should only erase 1 byte (position 2)
    std::vector<unsigned char> expected = {1, 2};
    VerifyFileContent(expected);
}

// Combined Operations
TEST_P(InsertEraseTest, InsertThenErase) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    // Insert data
    compio_seek(file, 2, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);
    ASSERT_EQ(inserted, 2);

    // Erase the inserted data
    compio_seek(file, 2, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(2, file);
    ASSERT_EQ(erased, 2);

    // Should be back to original
    std::vector<unsigned char> expected = {1, 2, 3, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, EraseThenInsert) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    // Erase some data
    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(2, file);
    ASSERT_EQ(erased, 2);

    // Insert new data
    compio_seek(file, 1, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);
    ASSERT_EQ(inserted, 2);

    std::vector<unsigned char> expected = {1, 9, 8, 4, 5};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, MultipleConsecutiveInserts) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    // First insert
    compio_seek(file, 1, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data1 = {9};
    uint64_t inserted1 = compio_insert(insert_data1.data(), insert_data1.size(), file);
    ASSERT_EQ(inserted1, 1);

    // Second insert at same position
    compio_seek(file, 1, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data2 = {8};
    uint64_t inserted2 = compio_insert(insert_data2.data(), insert_data2.size(), file);
    ASSERT_EQ(inserted2, 1);

    std::vector<unsigned char> expected = {1, 8, 9, 2, 3};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, MultipleConsecutiveErases) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    // First erase
    compio_seek(file, 1, COMPIO_SEEK_SET);
    uint64_t erased1 = compio_erase(1, file);
    ASSERT_EQ(erased1, 1);

    // Second erase at same position
    uint64_t erased2 = compio_erase(1, file);
    ASSERT_EQ(erased2, 1);

    std::vector<unsigned char> expected = {1, 4, 5};
    VerifyFileContent(expected);
}

// Position and Cursor Behavior
TEST_P(InsertEraseTest, CursorPositionAfterInsert) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8};
    compio_insert(insert_data.data(), insert_data.size(), file);

    // Cursor should be after inserted data
    uint64_t pos = compio_tell(file);
    ASSERT_EQ(pos, 3); // Position 1 + 2 inserted bytes
}

TEST_P(InsertEraseTest, CursorPositionAfterErase) {
    std::vector<unsigned char> initial = {1, 2, 3, 4, 5};
    WriteInitialData(initial);

    compio_seek(file, 1, COMPIO_SEEK_SET);
    compio_erase(2, file);

    // Cursor should stay at same position
    uint64_t pos = compio_tell(file);
    ASSERT_EQ(pos, 1);
}

// Block Granularity Tests
TEST_P(InsertEraseTest, MultiBlockFileInsert) {
    // Create file with many small writes to create multiple blocks
    std::vector<unsigned char> data1 = {1, 2};
    std::vector<unsigned char> data2 = {3, 4};
    std::vector<unsigned char> data3 = {5, 6};

    compio_write(data1.data(), data1.size(), file);
    compio_write(data2.data(), data2.size(), file);
    compio_write(data3.data(), data3.size(), file);

    // Insert in the middle (should span blocks)
    compio_seek(file, 3, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8, 7};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, 3);
    std::vector<unsigned char> expected = {1, 2, 3, 9, 8, 7, 4, 5, 6};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, MultiBlockFileErase) {
    // Create file with many small writes to create multiple blocks
    std::vector<unsigned char> data1 = {1, 2};
    std::vector<unsigned char> data2 = {3, 4};
    std::vector<unsigned char> data3 = {5, 6};

    compio_write(data1.data(), data1.size(), file);
    compio_write(data2.data(), data2.size(), file);
    compio_write(data3.data(), data3.size(), file);

    // Erase across block boundaries
    compio_seek(file, 2, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(3, file);

    ASSERT_EQ(erased, 3);
    std::vector<unsigned char> expected = {1, 2, 6};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, MultiBlockFileErase2) {
    std::vector<unsigned char> data1 = {1, 2, 3, 4, 5};
    std::vector<unsigned char> data2 = {5, 6, 7, 8, 9};
    std::vector<unsigned char> data3 = {10, 11, 12, 13, 14};

    compio_write(data1.data(), data1.size(), file);
    compio_write(data2.data(), data2.size(), file);
    compio_write(data3.data(), data3.size(), file);

    compio_seek(file, 3, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(9, file);

    ASSERT_EQ(erased, 9);
    std::vector<unsigned char> expected = {1, 2, 3, 12, 13, 14};
    VerifyFileContent(expected);
}

TEST_P(InsertEraseTest, WriteAtEndAppendsIntoTailBlockWhenFits) {
    std::vector<unsigned char> initial = {1, 2, 3};
    WriteInitialData(initial);
    ASSERT_EQ(GetBlockCount(), 1u);

    compio_seek(file, 3, COMPIO_SEEK_SET);
    std::vector<unsigned char> append = {4, 5};
    uint64_t written = compio_write(append.data(), append.size(), file);

    ASSERT_EQ(written, append.size());
    std::vector<unsigned char> expected = {1, 2, 3, 4, 5};
    VerifyFileContent(expected);
    ASSERT_EQ(GetBlockCount(), 1u);
}

TEST_P(InsertEraseTest, InsertInMiddleReusesBlockWhenFitsMaximum) {
    std::vector<unsigned char> initial = {1, 2, 3, 4};
    WriteInitialData(initial);
    ASSERT_EQ(GetBlockCount(), 1u);

    compio_seek(file, 2, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8, 7};
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, insert_data.size());
    std::vector<unsigned char> expected = {1, 2, 9, 8, 7, 3, 4};
    VerifyFileContent(expected);
    ASSERT_EQ(GetBlockCount(), 1u);
}

TEST_F(FragmentationReuseTest, InsertInMiddleRepacksBlockWhenExceedingMaximum) {
    std::vector<unsigned char> initial = {1, 2, 3, 4};
    ASSERT_EQ(compio_write(initial.data(), initial.size(), file), initial.size());
    ASSERT_EQ(GetBlockCount(), 1u);

    compio_seek(file, 2, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data = {9, 8, 7, 6, 5};
    ASSERT_EQ(compio_insert(insert_data.data(), insert_data.size(), file), insert_data.size());

    std::vector<unsigned char> expected = {1, 2, 9, 8, 7, 6, 5, 3, 4};
    VerifyFileContent(expected);
    ASSERT_EQ(GetBlockCount(), 2u);
}

// Parameterized tests for different data sizes
TEST_P(InsertEraseParamTest, ParametrizedInsertTest) {
    auto [initial_size, insert_pos, insert_size] = GetParam();

    // Create initial data
    std::vector<unsigned char> initial_data(initial_size);
    for (size_t i = 0; i < initial_size; ++i) {
        initial_data[i] = static_cast<unsigned char>(i % 256);
    }
    WriteInitialData(initial_data);

    // Create insert data
    std::vector<unsigned char> insert_data(insert_size);
    for (size_t i = 0; i < insert_size; ++i) {
        insert_data[i] = static_cast<unsigned char>((i + 100) % 256);
    }

    // Perform insert
    uint64_t pos = std::min(insert_pos, initial_size);
    compio_seek(file, pos, COMPIO_SEEK_SET);
    uint64_t inserted = compio_insert(insert_data.data(), insert_size, file);

    ASSERT_EQ(inserted, insert_size);

    // Verify result
    std::vector<unsigned char> expected;
    expected.insert(expected.end(), initial_data.begin(), initial_data.begin() + pos);
    expected.insert(expected.end(), insert_data.begin(), insert_data.end());
    expected.insert(expected.end(), initial_data.begin() + pos, initial_data.end());

    VerifyFileContent(expected);
}

TEST_P(InsertEraseParamTest, ParametrizedEraseTest) {
    auto [initial_size, erase_pos, erase_size] = GetParam();

    // Create initial data
    std::vector<unsigned char> initial_data(initial_size);
    for (size_t i = 0; i < initial_size; ++i) {
        initial_data[i] = static_cast<unsigned char>(i % 256);
    }
    WriteInitialData(initial_data);

    // Perform erase
    size_t pos = std::min(erase_pos, initial_size);
    size_t actual_erase_size = std::min(erase_size, initial_size - pos);
    compio_seek(file, pos, COMPIO_SEEK_SET);
    uint64_t erased = compio_erase(erase_size, file);

    ASSERT_EQ(erased, actual_erase_size);

    // Verify result
    std::vector<unsigned char> expected;
    expected.insert(expected.end(), initial_data.begin(), initial_data.begin() + pos);
    expected.insert(expected.end(), initial_data.begin() + pos + actual_erase_size,
                    initial_data.end());

    VerifyFileContent(expected);
}

// Instantiate parameterized tests with various combinations
INSTANTIATE_TEST_SUITE_P(InsertEraseParamTests, InsertEraseParamTest,
                        ::testing::Values(
                            // Small files
                            std::make_tuple(0, 0, 1),  // empty file, insert at start
                            std::make_tuple(0, 0, 10), // empty file, insert larger data
                            std::make_tuple(1, 0, 1),  // single byte, insert at start
                            std::make_tuple(1, 1, 1),  // single byte, insert at end
                            std::make_tuple(5, 2, 3),  // small file, insert in middle
                            std::make_tuple(5, 0, 5),  // small file, insert at start
                            std::make_tuple(5, 5, 5),  // small file, insert at end

                            // Medium files
                            std::make_tuple(50, 10, 5),  // medium file, insert in middle
                            std::make_tuple(50, 25, 25), // medium file, insert at middle
                            std::make_tuple(50, 0, 50),  // medium file, insert at start
                            std::make_tuple(50, 50, 50), // medium file, insert at end

                            // Large files
                            std::make_tuple(1000, 100, 50),   // large file, insert in middle
                            std::make_tuple(1000, 500, 100),  // large file, insert at middle
                            std::make_tuple(1000, 0, 200),    // large file, insert at start
                            std::make_tuple(1000, 1000, 200), // large file, insert at end

                            // Edge cases
                            std::make_tuple(10, 5, 0),  // zero size insert
                            std::make_tuple(10, 10, 5), // insert at end
                            std::make_tuple(10, 15, 5), // insert beyond end

                            // Erase specific cases
                            std::make_tuple(10, 0, 5),  // erase from start
                            std::make_tuple(10, 5, 3),  // erase from middle
                            std::make_tuple(10, 8, 5),  // erase beyond end
                            std::make_tuple(10, 10, 5), // erase at end
                            std::make_tuple(10, 0, 10), // erase entire file
                            std::make_tuple(10, 0, 15)  // erase more than file size
                            ));

// Test with different block configurations
class InsertEraseBlockConfigTest : public ::testing::TestWithParam<int> {
protected:
    compio_config config;
    compio_archive *archive;
    compio_file *file;
    char fn[256];
    bool failed = false;

    void SetUp() override {
        compio_build_default_config(&config);
        config.block_size = GetParam();
        config.block_size__minimum = config.block_size / 2;
        config.block_size__maximum = config.block_size * 2;

        generate_tmp_fn(fn, sizeof(fn));

        archive = compio_open_archive(fn, "w+", &config);
        if (!archive) {
            failed = true;
            FAIL() << "failed to open archive";
        }
        file = compio_open_file("test_file", archive);
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

        remove(fn); remove((std::string(fn) + ".wal").c_str());
    }
};

TEST_P(InsertEraseBlockConfigTest, InsertAcrossBlockBoundaries) {
    int block_size = GetParam();

    // Create data that spans multiple blocks
    std::vector<unsigned char> initial_data(block_size * 2);
    for (int i = 0; i < block_size * 2; ++i) {
        initial_data[i] = static_cast<unsigned char>(i % 256);
    }
    compio_write(initial_data.data(), initial_data.size(), file);

    // Insert at block boundary
    compio_seek(file, block_size, COMPIO_SEEK_SET);
    std::vector<unsigned char> insert_data(block_size / 2);
    for (int i = 0; i < block_size / 2; ++i) {
        insert_data[i] = static_cast<unsigned char>((i + 200) % 256);
    }
    uint64_t inserted = compio_insert(insert_data.data(), insert_data.size(), file);

    ASSERT_EQ(inserted, insert_data.size());

    // Verify result
    std::vector<unsigned char> expected;
    expected.insert(expected.end(), initial_data.begin(), initial_data.begin() + block_size);
    expected.insert(expected.end(), insert_data.begin(), insert_data.end());
    expected.insert(expected.end(), initial_data.begin() + block_size, initial_data.end());

    uint64_t file_size = compio_get_size(file);
    std::vector<unsigned char> actual(file_size);
    compio_seek(file, 0, COMPIO_SEEK_SET);
    compio_read(actual.data(), file_size, file);

    ASSERT_EQ(actual, expected);
}

INSTANTIATE_TEST_SUITE_P(InsertEraseBlockConfigTests, InsertEraseBlockConfigTest,
                        ::testing::Values(4, 8, 16, 32, 64, 128, 256));

INSTANTIATE_TEST_SUITE_P(InsertEraseCacheTests, InsertEraseTest, ::testing::Values(16, 0));
