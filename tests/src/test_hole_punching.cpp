#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

class HolePunchingTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    static uint64_t file_size(const char *path) { return std::filesystem::file_size(path); }

    // Storage the file takes on disk, as opposed to its size.
    static uint64_t disk_usage(const char *path) {
#ifdef _WIN32
        // The allocated ranges inside the file are added up. GetCompressedFileSize
        // would also count what NTFS allocates in advance past the end of a file
        // that grows by small writes and keeps until the file is closed: up to a
        // few megabytes, different from run to run.
        const HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
        EXPECT_NE(handle, INVALID_HANDLE_VALUE);
        if (handle == INVALID_HANDLE_VALUE) return 0;

        const uint64_t size = file_size(path);
        std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges(1024);
        uint64_t usage = 0;
        uint64_t position = 0;
        bool more = true;
        while (more && position < size) {
            FILE_ALLOCATED_RANGE_BUFFER query;
            query.FileOffset.QuadPart = static_cast<LONGLONG>(position);
            query.Length.QuadPart = static_cast<LONGLONG>(size - position);
            DWORD bytes = 0;
            const BOOL ok = DeviceIoControl(handle, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query), ranges.data(),
                                            static_cast<DWORD>(ranges.size() * sizeof(FILE_ALLOCATED_RANGE_BUFFER)),
                                            &bytes, nullptr);
            more = !ok && GetLastError() == ERROR_MORE_DATA;
            EXPECT_TRUE(ok || more);
            const size_t count = bytes / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            if (count == 0) break;
            for (size_t i = 0; i < count; i++) {
                usage += static_cast<uint64_t>(ranges[i].Length.QuadPart);
            }
            position = static_cast<uint64_t>(ranges[count - 1].FileOffset.QuadPart + ranges[count - 1].Length.QuadPart);
        }
        CloseHandle(handle);
        return usage;
#else
        struct stat st;
        EXPECT_EQ(stat(path, &st), 0);
        return static_cast<uint64_t>(st.st_blocks) * 512;
#endif
    }

    static bool sync_to_disk(FILE *file) {
        if (fflush(file) != 0) return false;
#ifdef _WIN32
        return _commit(_fileno(file)) == 0;
#else
        return fsync(fileno(file)) == 0;
#endif
    }

    // Probes the filesystem the test files live on with the library's own
    // primitive. The runners of the CI use filesystems that do punch holes, so
    // there a negative answer is a defect of that primitive, not a reason to skip.
    bool filesystem_punches_holes() {
        constexpr size_t PROBE_BYTES = 1 << 20;
        FILE *file = fopen(fn, "wb+");
        if (!file) return false;
        const std::vector<char> data(PROBE_BYTES, 'x');
        bool ok = fwrite(data.data(), 1, data.size(), file) == data.size() && sync_to_disk(file);
        const uint64_t before = ok ? disk_usage(fn) : 0;
        ok = ok && compio::punch_file_hole(file, 0, PROBE_BYTES) && sync_to_disk(file);
        fclose(file);
        ok = ok && disk_usage(fn) + PROBE_BYTES / 2 < before;
        remove(fn);
        return ok;
    }

    static std::vector<uint8_t> incompressible(size_t size, uint32_t seed) {
        std::mt19937 rng(seed);
        std::vector<uint8_t> data(size);
        for (auto &b : data) b = static_cast<uint8_t>(rng());
        return data;
    }
};

#define SKIP_WITHOUT_HOLE_PUNCHING()                                                               \
    if (!filesystem_punches_holes()) {                                                            \
        ASSERT_EQ(getenv("GITHUB_ACTIONS"), nullptr) << "hole punching must work on the CI runners"; \
        GTEST_SKIP() << "filesystem does not support hole punching";                              \
    }

TEST_F(HolePunchingTest, RemovedFileStopsOccupyingDisk) {
    SKIP_WITHOUT_HOLE_PUNCHING();

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
    EXPECT_LT(disk_usage(fn), usage_before - FILE_BYTES * 9 / 10) << "usage before " << usage_before;

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
    SKIP_WITHOUT_HOLE_PUNCHING();

    constexpr uint64_t REGION = 1000;
    constexpr int COUNT = 4096;
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
        ASSERT_EQ(compio::fseek64(ar->file, static_cast<int64_t>(offset), SEEK_SET), 0);
        ASSERT_EQ(fwrite(filler.data(), 1, REGION, ar->file), REGION);
        offsets.push_back(offset);
    }
    ASSERT_TRUE(sync_to_disk(ar->file));
    const uint64_t usage_before = disk_usage(fn);

    for (const uint64_t offset : offsets) {
        ar->allocator->deallocate(offset, REGION, false);
    }
    ASSERT_TRUE(sync_to_disk(ar->file));

    const uint64_t freed = REGION * COUNT;
    EXPECT_LT(disk_usage(fn), usage_before - freed * 9 / 10)
        << "usage before " << usage_before << ", freed " << freed;

    compio_close_archive(ar);
}
