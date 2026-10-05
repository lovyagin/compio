#include <gtest/gtest.h>

#ifdef __linux__

#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "test_util.hpp"

class HolePunchingTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    static uint64_t disk_usage(const char *path) {
        struct stat st;
        EXPECT_EQ(stat(path, &st), 0);
        return static_cast<uint64_t>(st.st_blocks) * 512;
    }

    static uint64_t file_size(const char *path) {
        struct stat st;
        EXPECT_EQ(stat(path, &st), 0);
        return static_cast<uint64_t>(st.st_size);
    }

    bool filesystem_punches_holes() {
        const int fd = open(fn, O_RDWR);
        if (fd < 0) return false;
        const std::vector<char> data(1 << 16, 'x');
        bool ok = write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()) &&
                  fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1 << 16) == 0;
        ok = ok && ftruncate(fd, 0) == 0;
        close(fd);
        return ok;
    }

    static std::vector<uint8_t> incompressible(size_t size, uint32_t seed) {
        std::mt19937 rng(seed);
        std::vector<uint8_t> data(size);
        for (auto &b : data) b = static_cast<uint8_t>(rng());
        return data;
    }
};

TEST_F(HolePunchingTest, RemovedFileStopsOccupyingDisk) {
    if (!filesystem_punches_holes()) GTEST_SKIP() << "filesystem does not support hole punching";

    constexpr size_t FILE_BYTES = 4 << 20;
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fill_holes_with_zeros = true;
    cfg.fragmentation_threshold = 100;

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    for (const char *name : {"keep", "drop"}) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr);
        const auto data = incompressible(FILE_BYTES, name[0]);
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        compio_close_file(f);
    }
    compio_flush(ar);
    const uint64_t size_before = file_size(fn);
    const uint64_t usage_before = disk_usage(fn);
    ASSERT_GE(usage_before, 2 * FILE_BYTES);

    ASSERT_EQ(compio_remove_file(ar, "drop"), 0);
    compio_flush(ar);

    // The container keeps its size, but the removed half no longer takes disk space.
    EXPECT_GE(file_size(fn), size_before);
    EXPECT_LT(disk_usage(fn), usage_before - FILE_BYTES * 9 / 10);

    compio_file *f = compio_open_file("keep", ar);
    ASSERT_NE(f, nullptr);
    std::vector<uint8_t> back(FILE_BYTES);
    ASSERT_EQ(compio_read(back.data(), back.size(), f), back.size());
    EXPECT_EQ(back, incompressible(FILE_BYTES, 'k'));
    compio_close_file(f);
    compio_close_archive(ar);
}

// Regions smaller than a filesystem block free nothing one by one; the blocks
// must be returned once neighbouring regions add up to cover them.
TEST_F(HolePunchingTest, AdjacentSmallRegionsAreReleasedTogether) {
    if (!filesystem_punches_holes()) GTEST_SKIP() << "filesystem does not support hole punching";

    constexpr uint64_t REGION = 1000;
    constexpr int COUNT = 256;
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    cfg.fill_holes_with_zeros = true;
    cfg.fragmentation_threshold = 100;

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);

    std::vector<uint64_t> offsets;
    const std::vector<uint8_t> filler(REGION, 0xAB);
    for (int i = 0; i < COUNT; i++) {
        const uint64_t offset = ar->allocator->allocate(REGION);
        ASSERT_NE(offset, UINT64_MAX);
        ASSERT_EQ(fseek(ar->file, static_cast<long>(offset), SEEK_SET), 0);
        ASSERT_EQ(fwrite(filler.data(), 1, REGION, ar->file), REGION);
        offsets.push_back(offset);
    }
    ASSERT_EQ(fflush(ar->file), 0);
    ASSERT_EQ(fsync(fileno(ar->file)), 0);
    const uint64_t usage_before = disk_usage(fn);

    for (const uint64_t offset : offsets) {
        ar->allocator->deallocate(offset, REGION, false);
    }
    ASSERT_EQ(fsync(fileno(ar->file)), 0);

    const uint64_t freed = REGION * COUNT;
    EXPECT_LT(disk_usage(fn), usage_before - freed * 9 / 10);

    compio_close_archive(ar);
}

#endif // __linux__
