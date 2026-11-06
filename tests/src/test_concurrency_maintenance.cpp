#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

namespace {


// Maintenance (flush, compaction, compressor switch, recompression, batches) runs while
// other threads write and read their own files.
class ConcurrencyMaintenanceTest : public ::testing::Test {
protected:
    static constexpr int writers = 4;
    static constexpr size_t file_size = 192 * 1024;

    char fn[256];
    compio_config config;
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.block_size__minimum = 1024;
        config.block_size__maximum = 8192;
        config.cache_size__blocks = 16;
        config.fragmentation_threshold = 10;
        compio_build_lz4_compressor(&config.compressor);
        archive = compio_open_archive(fn, "w+", &config);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
    }

    static std::string name_of(int writer) { return "w" + std::to_string(writer); }

    // Rewrites random ranges of the writer's file and keeps a shadow copy to compare with.
    void writer_loop(int writer, std::atomic<bool> &run, std::atomic<int> &ready, std::atomic<int> &errors, std::vector<uint8_t> &shadow) {
        std::mt19937 rng(1000u + writer);
        shadow.assign(file_size, 0);
        compio_file *file = compio_open_file(name_of(writer).c_str(), archive);
        if (!file) {
            ++errors;
            return;
        }
        if (compio_write(shadow.data(), shadow.size(), file) != shadow.size()) ++errors;
        ++ready;

        while (run.load()) {
            const size_t len = 512 + rng() % 12000;
            const size_t pos = rng() % (file_size - len);
            std::vector<uint8_t> chunk(len);
            // Compressible and incompressible data alternate so both stored and packed blocks appear.
            const bool noisy = rng() % 2;
            for (auto &b : chunk) b = noisy ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>(pos / 64);

            if (compio_seek(file, static_cast<int64_t>(pos), 0) != 0 ||
                compio_write(chunk.data(), len, file) != len) {
                ++errors;
                continue;
            }
            std::copy(chunk.begin(), chunk.end(), shadow.begin() + pos);

            std::vector<uint8_t> back(len);
            if (compio_seek(file, static_cast<int64_t>(pos), 0) != 0 ||
                compio_read(back.data(), len, file) != len || back != chunk) {
                ++errors;
            }
        }
        if (compio_close_file(file) != 0) ++errors;
    }

    void reader_loop(std::atomic<bool> &run, std::atomic<int> &ready, std::atomic<int> &errors) {
        std::mt19937 rng(77);
        while (run.load() && ready.load() < writers) std::this_thread::yield();
        while (run.load()) {
            compio_file *file = compio_open_file(name_of(rng() % writers).c_str(), archive);
            if (!file) {
                ++errors;
                continue;
            }
            std::vector<uint8_t> buf(4096);
            if (compio_seek(file, static_cast<int64_t>(rng() % (file_size - buf.size())), 0) != 0 ||
                compio_read(buf.data(), buf.size(), file) != buf.size()) {
                ++errors;
            }
            compio_close_file(file);
        }
    }

    void maintenance_loop(std::atomic<bool> &run, std::atomic<int> &errors) {
        const compio_compression_type kinds[] = {COMPIO_COMPRESS_ZSTD, COMPIO_COMPRESS_ZLIB, COMPIO_COMPRESS_LZ4,
                                                 COMPIO_COMPRESS_DUMMY};
        std::mt19937 rng(5);
        compio_compressor c;
        while (run.load()) {
            switch (rng() % 6) {
            case 0:
                compio_flush(archive);
                break;
            case 1:
                if (compio_defragment(archive) != COMPIO_SUCCESS) ++errors;
                break;
            case 2:
                if (compio_build_compressor_by_type(&c, kinds[rng() % 4]) != 0 ||
                    compio_set_compressor(archive, &c) != COMPIO_SUCCESS) {
                    ++errors;
                }
                break;
            case 3: {
                uint64_t rewritten = 0;
                const int rc = compio_recompress(archive, &rewritten);
                if (rc != COMPIO_SUCCESS && errno != EBUSY) ++errors;
                break;
            }
            case 4: {
                compio_fragmentation_stats stats;
                if (compio_get_fragmentation_stats(archive, &stats) != COMPIO_SUCCESS) ++errors;
                break;
            }
            default: {
                // Scratch file churn so the allocator has holes to compact.
                compio_file *f = compio_open_file("scratch", archive);
                if (f) {
                    std::vector<uint8_t> junk(20000, static_cast<uint8_t>(rng()));
                    compio_write(junk.data(), junk.size(), f);
                    compio_close_file(f);
                    compio_remove_file(archive, "scratch");
                } else {
                    ++errors;
                }
            }
            }
        }
    }
};

TEST_F(ConcurrencyMaintenanceTest, WritersSurviveMaintenance) {
    std::atomic<bool> run{true};
    std::atomic<int> errors{0};
    std::atomic<int> ready{0};
    std::vector<std::vector<uint8_t>> shadows(writers);

    std::vector<std::thread> workers;
    for (int i = 0; i < writers; ++i) {
        workers.emplace_back([&, i] { writer_loop(i, run, ready, errors, shadows[i]); });
    }
    workers.emplace_back([&] { reader_loop(run, ready, errors); });
    workers.emplace_back([&] { maintenance_loop(run, errors); });

    std::this_thread::sleep_for(std::chrono::seconds(6));
    run = false;
    for (auto &t : workers) t.join();

    EXPECT_EQ(errors.load(), 0);

    compio_flush(archive);
    for (int i = 0; i < writers; ++i) {
        compio_file *file = compio_open_file(name_of(i).c_str(), archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_get_size(file), shadows[i].size());
        std::vector<uint8_t> back(shadows[i].size());
        ASSERT_EQ(compio_read(back.data(), back.size(), file), back.size());
        EXPECT_TRUE(back == shadows[i]) << "writer " << i;
        compio_close_file(file);
    }
}

} // namespace
