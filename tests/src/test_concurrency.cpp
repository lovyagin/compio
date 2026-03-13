#include <gtest/gtest.h>
#include <compio.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <filesystem>

namespace fs = std::filesystem;

class ConcurrencyTest : public ::testing::Test {
protected:
    std::string test_file = "concurrency_test.compio";
    compio_archive* archive = nullptr;

    void SetUp() override {
        if (fs::exists(test_file)) {
            fs::remove(test_file);
        }
        compio_config config;
        compio_build_default_config(&config);
        config.block_size = 4096;
        archive = compio_open_archive(test_file.c_str(), "w+", &config);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) {
            compio_close_archive(archive);
            archive = nullptr;
        }
        if (fs::exists(test_file)) {
            fs::remove(test_file);
        }
    }
};

TEST_F(ConcurrencyTest, ConcurrentReadWriteDifferentFiles) {
    const int num_files = 10;
    const int num_threads = 4;
    const int ops_per_thread = 100;
    std::atomic<bool> running{true};

    // Pre-create files
    for (int i = 0; i < num_files; ++i) {
        std::string fname = "file_" + std::to_string(i);
        compio_file* f = compio_open_file(fname.c_str(), archive);
        ASSERT_NE(f, nullptr);
        compio_close_file(f);
    }

    auto worker = [&](int id) {
        std::mt19937 rng(id);
        std::vector<uint8_t> buffer(1024);
        
        for (int i = 0; i < ops_per_thread; ++i) {
            int file_idx = rng() % num_files;
            std::string fname = "file_" + std::to_string(file_idx);
            
            // Open file (thread-safe now)
            compio_file* f = compio_open_file(fname.c_str(), archive);
            if (!f) continue;

            // Write or Read
            if (rng() % 2 == 0) {
                // Write
                std::fill(buffer.begin(), buffer.end(), (uint8_t)rng());
                compio_write(buffer.data(), buffer.size(), f);
            } else {
                // Read
                compio_read(buffer.data(), buffer.size(), f);
            }

            compio_close_file(f);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }
}

TEST_F(ConcurrencyTest, ConcurrentReadSameFile) {
    const std::string fname = "shared_file";
    {
        compio_file* f = compio_open_file(fname.c_str(), archive);
        std::vector<uint8_t> data(1024 * 1024, 'A');
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    }

    const int num_readers = 4;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_readers; ++i) {
        threads.emplace_back([&]() {
            compio_file* f = compio_open_file(fname.c_str(), archive);
            ASSERT_NE(f, nullptr);
            std::vector<uint8_t> buf(1024);
            for(int j=0; j<100; ++j) {
                compio_read(buf.data(), buf.size(), f);
            }
            compio_close_file(f);
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}
