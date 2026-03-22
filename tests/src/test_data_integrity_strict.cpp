#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/file.hpp"
#include "test_util.hpp"

class DataIntegrityStrictTest : public ::testing::Test {
protected:
    char fn[256];
    std::string wal_fn;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        wal_fn = std::string(fn) + ".wal";
    }

    void TearDown() override {
        remove(fn);
        remove(wal_fn.c_str());
    }

    void CreateArchiveWithData(const std::string& filename, const std::string& content) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = 10;
        cfg.block_size = 4096;
        
        // Use dummy compression to ensure data is plaintext on disk
        // This makes corruption deterministic (we can find the content string)
        compio_build_dummy_compressor(&cfg.compressor);

        compio_archive* ar = compio_open_archive(fn, "w+", &cfg);
        ASSERT_NE(ar, nullptr);

        compio_file* f = compio_open_file(filename.c_str(), ar);
        ASSERT_NE(f, nullptr);

        ASSERT_EQ(compio_write(content.data(), content.size(), f), content.size());

        compio_close_file(f);
        compio_close_archive(ar);
    }
};

TEST_F(DataIntegrityStrictTest, ReadFailsOnCorruptedBlock) {
    const std::string filename = "test.txt";
    const std::string content = "This is some test data that will be corrupted.";
    CreateArchiveWithData(filename, content);

    // Corrupt the data on disk
    FILE* f = fopen(fn, "r+b");
    ASSERT_NE(f, nullptr);

    std::vector<uint8_t> file_content;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    file_content.resize(fsize);
    fread(file_content.data(), 1, fsize, f);

    // Find content
    size_t found_pos = std::string(file_content.begin(), file_content.end()).find(content);
    ASSERT_NE(found_pos, std::string::npos) << "Could not find content in archive file (compression might be interfering)";

    // Corrupt one byte of the content
    fseek(f, found_pos, SEEK_SET);
    uint8_t byte;
    fread(&byte, 1, 1, f);
    byte ^= 0xFF; // Flip bits
    fseek(f, found_pos, SEEK_SET);
    fwrite(&byte, 1, 1, f);
    fclose(f);

    // Now try to read it back
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 10; // Match creation config
    
    // Use dummy compressor for reading too
    compio_build_dummy_compressor(&cfg.compressor);
    
    compio_archive* ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);

    compio_file* cf = compio_open_file(filename.c_str(), ar);
    ASSERT_NE(cf, nullptr);

    char buffer[1024];
    // This should fail now!
    
    // Reset errno
    errno = 0;
    uint64_t bytes_read = compio_read(buffer, sizeof(buffer), cf);
    
    EXPECT_EQ(bytes_read, 0) << "Should not read corrupted data";
    EXPECT_EQ(errno, EIO) << "Errno should be set to EIO";

    compio_close_file(cf);
    compio_close_archive(ar);
}
