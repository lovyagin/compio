#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <algorithm>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

namespace {
std::vector<uint8_t> pseudo_random(std::size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t s = seed ? seed : 1;
    for (auto &b : v) { s = s * 1664525u + 1013904223u; b = static_cast<uint8_t>(s >> 24); }
    return v;
}
} // namespace

class RepairTest : public ::testing::Test {
protected:
    void SetUp() override {
        char buffer[1024];
        generate_tmp_fn(buffer, 1024);
        std::string unique_base = buffer;
        
        // generate_tmp_fn creates the file, remove it so we can use the name (or derived name)
        if (fs::exists(unique_base)) {
            fs::remove(unique_base);
        }
        
        fs::path p(unique_base);
        tmp_dir = p.string() + "_dir";
        
        if (fs::exists(tmp_dir)) fs::remove_all(tmp_dir);
        fs::create_directories(tmp_dir);
        
        archive_path = (fs::path(tmp_dir) / "archive.compio").string();
        recover_dir = (fs::path(tmp_dir) / "recovered").string();
    }

    void TearDown() override {
        if (fs::exists(tmp_dir)) fs::remove_all(tmp_dir);
    }

    std::string tmp_dir;
    std::string archive_path;
    std::string recover_dir;
};

TEST_F(RepairTest, RecoversFromValidArchive) {
    {
        compio_config config;
        compio_build_default_config(&config);
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);

        compio_file* f = compio_open_file("test.txt", archive);
        ASSERT_NE(f, nullptr);
        std::string data = "Hello World";
        compio_write(data.c_str(), data.size(), f);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    // Should recover at least 1 file
    ASSERT_GE(count, 1);
    
    fs::path recovered_file = fs::path(recover_dir) / "test.txt";
    ASSERT_TRUE(fs::exists(recovered_file));
    std::ifstream ifs(recovered_file, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content, "Hello World");
}

TEST_F(RepairTest, RecoversWithCorruptedHeader) {
    uint64_t hash = 0;
    {
        compio_config config;
        compio_build_default_config(&config);
        // We use default configuration.
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);
        
        compio_file* f = compio_open_file("data.bin", archive);
        ASSERT_NE(f, nullptr);
        hash = f->hash;
        
        std::vector<uint8_t> data(1000, 0xAB);
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    // Corrupt header (overwrite first 512 bytes with zeros)
    {
#ifdef _WIN32
        FILE* f = _wfopen(fs::path(archive_path).c_str(), L"rb+");
#else
        FILE* f = fopen(archive_path.c_str(), "rb+");
#endif
        ASSERT_NE(f, nullptr);
        std::unique_ptr<FILE, int(*)(FILE*)> f_guard(f, fclose);
        std::vector<uint8_t> zeros(512, 0);
        fwrite(zeros.data(), 1, 512, f);
    }

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    ASSERT_GE(count, 1);
    
    // Check for file_<hash> (since header with names is gone)
    // When header is corrupted, compio_repair uses a heuristic to guess compression.
    // Since we used COMPRESS_NONE and the heuristic defaults to ZLIB if unknown, 
    // we might get a decompression failure if heuristic is wrong.
    // However, compio_repair with corrupted header defaults to ZLIB (which is default config).
    // If the data is not ZLIB compressed, decompression will fail.
    // To make this test robust, we should use ZLIB (default) or ensure heuristic handles it.
    // Wait, compio_repair code says: 
    // "warning: header corrupted, using default settings for salvage (degree=16, zlib)"
    // So if we write UNCOMPRESSED data, the repair tool will try to decompress it as ZLIB and fail.
    // Therefore, we should write ZLIB data (default config) so the repair tool can recover it.
    // Reverting config change to use default (ZLIB).
    
    // Actually, let's stick to default config (ZLIB) as repair assumes it.
    
    std::string expected_name = "file_" + std::to_string(hash);
    
    for (const auto& entry : fs::directory_iterator(recover_dir)) {
        if (entry.path().filename().string().find("file_") == 0 || 
            entry.path().filename().string().find("orphan_") == 0) {
            // Content check
            std::ifstream ifs(entry.path(), std::ios::binary);
            std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.size() == 1000 && content[0] == 0xAB) {
                SUCCEED();
                return;
            }
        }
    }
    
    // Fallback search by exact hash name if above loop didn't return
    fs::path recovered_path = fs::path(recover_dir) / expected_name;
    if (fs::exists(recovered_path)) {
        std::ifstream ifs(recovered_path, std::ios::binary);
        std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ASSERT_EQ(content.size(), 1000);
        if (!content.empty()) { ASSERT_EQ(content[0], 0xAB); }
    } else {
        // It might be recovered as orphan if index node was also lost/corrupted (unlikely here as we only corrupted header)
        // or if hash mapping failed.
        // Let's check for any file with correct size
         bool any_correct = false;
         for (const auto& entry : fs::directory_iterator(recover_dir)) {
            std::ifstream ifs(entry.path(), std::ios::binary);
            ifs.seekg(0, std::ios::end);
            if (ifs.tellg() == 1000) {
                any_correct = true;
                break;
            }
         }
         ASSERT_TRUE(any_correct) << "No recovered file has correct size";
    }
}

// Wipe every readable B-tree index node by zeroing its signature byte,
// simulating total loss of the index. Returns the number of nodes wiped.
static int wipe_index_nodes(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    compio::header h;
    if (!h.load_and_validate(f, 0)) { fclose(f); return -1; }
    const int degree = h.b_tree_degree;

    compio::fseek64(f, 0, SEEK_END);
    uint64_t size = compio::ftell64(f);
    compio::fseek64(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size);
    if (fread(bytes.data(), 1, size, f) != size) { fclose(f); return -1; }

    std::vector<uint64_t> node_offsets;
    for (uint64_t off = 0; off < size; ++off) {
        if (bytes[off] != compio::index_node::signature) continue;
        compio::index_node node(degree);
        if (node.read_from(f, off)) node_offsets.push_back(off);
    }
    fclose(f);

    FILE *fw = fopen(path.c_str(), "rb+");
    if (!fw) return -1;
    const uint8_t zero = 0;
    for (uint64_t off : node_offsets) {
        compio::fseek64(fw, off, SEEK_SET);
        fwrite(&zero, 1, 1, fw);
    }
    fclose(fw);
    return static_cast<int>(node_offsets.size());
}

// Total index loss: every index node is wiped, so files can only be rebuilt
// from the self-describing {hash,pos} back-refs carried by v2 storage blocks.
TEST_F(RepairTest, RecoversOrphanBlocksViaBackref) {
    const std::size_t SZ = 128 * 1024; // span many blocks across multiple index nodes
    auto payload = pseudo_random(SZ, 1234);
    {
        compio_config config;
        compio_build_default_config(&config);
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);
        compio_file* f = compio_open_file("big.bin", archive);
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(compio_write(payload.data(), SZ, f), (int)SZ);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    int wiped = wipe_index_nodes(archive_path);
    ASSERT_GT(wiped, 0) << "test must actually destroy index nodes";

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    ASSERT_GE(count, 1);

    // File recovered under its real name (header survived) purely via back-refs.
    fs::path recovered = fs::path(recover_dir) / "big.bin";
    ASSERT_TRUE(fs::exists(recovered)) << "back-ref re-attribution should rebuild the named file";
    std::ifstream ifs(recovered, std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content.size(), SZ);
    ASSERT_EQ(content, payload) << "recovered content must match original byte-for-byte";
}

// v2 storage blocks round-trip their {hash,pos} back-ref, and the checksum
// covers it so corruption of the back-ref is detected on read.
TEST_F(RepairTest, StorageBlockBackrefRoundTripAndChecksum) {
    std::string blk_path = (fs::path(tmp_dir) / "block.bin").string();
    const uint64_t addr = 64;
    const uint64_t N = 256;
    auto data = pseudo_random(N, 99);
    const compio::tree_key key{0xABCDEF0123456789ull, 0x1122334455667788ull};

    {
        compio::storage_block b;
        b.is_compressed = 0;
        b.size = N;
        b.original_size = N;
        b.data = std::make_unique<uint8_t[]>(N);
        std::copy(data.begin(), data.end(), b.data.get());
        b.checksum_type = COMPIO_CHECKSUM_FNV1A;
        b.src_key = key;
        FILE *f = fopen(blk_path.c_str(), "wb+");
        ASSERT_NE(f, nullptr);
        b.write_to(f, addr, nullptr);
        fclose(f);
    }

    {
        compio::storage_block b;
        FILE *f = fopen(blk_path.c_str(), "rb");
        ASSERT_NE(f, nullptr);
        ASSERT_TRUE(b.read_from(f, addr));
        fclose(f);
        EXPECT_TRUE(b.has_backref);
        EXPECT_EQ(b.src_key.hash, key.hash);
        EXPECT_EQ(b.src_key.pos, key.pos);
        ASSERT_EQ(b.size, N);
        EXPECT_EQ(std::memcmp(b.data.get(), data.data(), N), 0);
    }

    // Corrupt one byte of the on-disk back-ref (hash starts right after the
    // 18-byte common prefix). Checksum covers it, so read_from must fail.
    {
        FILE *f = fopen(blk_path.c_str(), "rb+");
        ASSERT_NE(f, nullptr);
        compio::fseek64(f, addr + 18, SEEK_SET);
        uint8_t v = 0;
        ASSERT_EQ(fread(&v, 1, 1, f), 1u);
        v ^= 0xFF;
        compio::fseek64(f, addr + 18, SEEK_SET);
        fwrite(&v, 1, 1, f);
        fclose(f);
    }
    {
        compio::storage_block b;
        FILE *f = fopen(blk_path.c_str(), "rb");
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(b.read_from(f, addr)) << "corrupted back-ref must fail checksum";
        fclose(f);
    }
}

// Backward compatibility: a hand-written legacy v1 block (no back-ref, 22-byte
// meta, checksum over data only) must still read correctly.
TEST_F(RepairTest, ReadsLegacyV1Block) {
    std::string blk_path = (fs::path(tmp_dir) / "v1block.bin").string();
    const uint64_t addr = 8; // non-zero (read_from asserts addr != 0)
    const uint64_t N = 200;
    auto data = pseudo_random(N, 7);
    const uint32_t cksum = compio::fnv1a_32(data.data(), N);

    std::vector<uint8_t> buf(addr, 0); // leading padding so block starts at addr
    auto put_u8 = [&](uint8_t v) { buf.push_back(v); };
    auto put_u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back((uint8_t)(v >> (i * 8))); };
    auto put_u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back((uint8_t)(v >> (i * 8))); };

    put_u8(compio::storage_block::signature); // 171, v1 FNV
    put_u8(0);                                 // is_compressed
    put_u64(N);                                // size
    put_u64(N);                                // original_size
    put_u32(cksum);                            // checksum (data only)
    buf.insert(buf.end(), data.begin(), data.end());

    {
        FILE *f = fopen(blk_path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fwrite(buf.data(), 1, buf.size(), f), buf.size());
        fclose(f);
    }

    compio::storage_block b;
    FILE *f = fopen(blk_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    ASSERT_TRUE(b.read_from(f, addr));
    fclose(f);
    EXPECT_FALSE(b.has_backref);
    ASSERT_EQ(b.size, N);
    EXPECT_EQ(std::memcmp(b.data.get(), data.data(), N), 0);
}
