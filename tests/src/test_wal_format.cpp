#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include "compio/wal.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

class WalFormatTest : public ::testing::Test {
protected:
    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));
        wal_filename = std::string(filename) + ".wal";
    }

    void TearDown() override {
        if (fs::exists(filename)) fs::remove(filename);
        if (fs::exists(wal_filename)) fs::remove(wal_filename);
    }

    char filename[256];
    std::string wal_filename;
};

TEST_F(WalFormatTest, EnforcesLittleEndianOnDisk) {
    compio::WalManager wal(filename);
    ASSERT_TRUE(wal.open());

    // Use a pattern that is distinct in LE vs BE
    // 0x1234567890ABCDEF
    // LE: EF CD AB 90 78 56 34 12
    // BE: 12 34 56 78 90 AB CD EF
    uint64_t addr = 0x1234567890ABCDEF;
    std::vector<uint8_t> data = {0xCC}; 

    wal.begin_transaction();
    ASSERT_TRUE(wal.log_write(compio::WalRecordType::BLOCK, addr, data.data(), data.size()));
    ASSERT_TRUE(wal.commit_transaction());
    wal.close();

    std::ifstream f(wal_filename, std::ios::binary);
    ASSERT_TRUE(f.is_open());

    // Record structure:
    // [Type(1)][Addr(8)][Size(8)][Checksum(4)]
    // ... data ...
    
    // Read Type
    char type;
    f.read(&type, 1);
    ASSERT_EQ(static_cast<uint8_t>(type), static_cast<uint8_t>(compio::WalRecordType::BLOCK));

    // Read Addr (8 bytes)
    uint8_t addr_bytes[8];
    f.read(reinterpret_cast<char*>(addr_bytes), 8);
    
    // Verify Little Endian
    ASSERT_EQ(addr_bytes[0], 0xEF);
    ASSERT_EQ(addr_bytes[1], 0xCD);
    ASSERT_EQ(addr_bytes[2], 0xAB);
    ASSERT_EQ(addr_bytes[3], 0x90);
    ASSERT_EQ(addr_bytes[4], 0x78);
    ASSERT_EQ(addr_bytes[5], 0x56);
    ASSERT_EQ(addr_bytes[6], 0x34);
    ASSERT_EQ(addr_bytes[7], 0x12);

    // Read Size (8 bytes)
    uint8_t size_bytes[8];
    f.read(reinterpret_cast<char*>(size_bytes), 8);
    
    // Verify Little Endian for size=1
    // 0x0000000000000001 -> 01 00 00 00 00 00 00 00
    ASSERT_EQ(size_bytes[0], 0x01);
    ASSERT_EQ(size_bytes[1], 0x00);
    ASSERT_EQ(size_bytes[7], 0x00);

    // Checksum (4 bytes) - skip verification as algorithm is complex
    f.seekg(4, std::ios::cur);

    // Data (1 byte)
    char data_byte;
    f.read(&data_byte, 1);
    ASSERT_EQ(static_cast<uint8_t>(data_byte), 0xCC);
}
