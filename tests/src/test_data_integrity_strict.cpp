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

    // Find the data block. 
    // Header is at 0. 
    // We need to parse header to find index root, then parse index to find data block?
    // Or just scan for the storage block signature (0xAB = 171).
    // Let's scan for signature.
    
    // Header size for max_files=10 is small.
    // 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 + 10*(32+8) = ~500 bytes.
    // Double buffered -> ~1000 bytes reserved.
    
    // We can search for the content string to be sure where the data is.
    
    std::vector<uint8_t> file_content;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    file_content.resize(fsize);
    fread(file_content.data(), 1, fsize, f);

    // Find content
    size_t found_pos = std::string(file_content.begin(), file_content.end()).find(content);
    ASSERT_NE(found_pos, std::string::npos) << "Could not find content in archive file";

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
    
    compio_archive* ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);

    compio_file* cf = compio_open_file(filename.c_str(), ar);
    ASSERT_NE(cf, nullptr);

    char buffer[1024];
    // This should fail now!
    // Before changes: it would warn and return corrupted data.
    // After changes: it should return 0 or partial read (if multiple blocks), and set errno.
    
    // Reset errno
    errno = 0;
    uint64_t bytes_read = compio_read(buffer, sizeof(buffer), cf);
    
    // If our logic works, bytes_read should be 0 because the only block is corrupted.
    // And errno should be EIO.
    EXPECT_EQ(bytes_read, 0) << "Should not read corrupted data";
    EXPECT_EQ(errno, EIO) << "Errno should be set to EIO";

    compio_close_file(cf);
    compio_close_archive(ar);
}
