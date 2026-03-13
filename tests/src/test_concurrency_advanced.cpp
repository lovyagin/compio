#include <gtest/gtest.h>
#include <compio.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <filesystem>
#include <string>
#include <mutex>
#include <algorithm>

namespace fs = std::filesystem;

class AdvancedConcurrencyTest : public ::testing::Test {
protected:
    std::string test_file;
    compio_archive* archive = nullptr;

    void SetUp() override {
        static std::atomic<int> counter{0};
        auto tmp_path = fs::temp_directory_path() / ("adv_concurrency_" + std::to_string(counter++) + ".compio");
        test_file = tmp_path.string();
        
        if (fs::exists(test_file)) {
            fs::remove(test_file);
        }
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

    void create_archive(int cache_blocks = 100) {
        compio_config config;
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.cache_size__blocks = cache_blocks;
        config.max_files = 10000; // Large limit for concurrency tests
        archive = compio_open_archive(test_file.c_str(), "w+", &config);
        ASSERT_NE(archive, nullptr);
    }
};

// Test 1: High Contention on Writes
// 8 threads, each creating 50 files and writing data.
// Verifies that archive mutex correctly serializes all write operations (Open/Write/Close).
TEST_F(AdvancedConcurrencyTest, ConcurrentWritersIntegrity) {
    create_archive();

    const int num_threads = 8;
    const int files_per_thread = 50;
    const size_t data_size = 1024; // 1KB

    std::atomic<int> errors{0};

    auto worker = [&](int id) {
        std::vector<uint8_t> buffer(data_size);
        for (int i = 0; i < files_per_thread; ++i) {
            // Fill buffer with pattern
            std::fill(buffer.begin(), buffer.end(), (uint8_t)(id * 10 + i));
            
            std::string fname = "file_" + std::to_string(id) + "_" + std::to_string(i);
            
            compio_file* f = compio_open_file(fname.c_str(), archive);
            if (!f) {
                errors++;
                continue;
            }

            if (compio_write(buffer.data(), buffer.size(), f) != data_size) {
                errors++;
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

    ASSERT_EQ(errors.load(), 0) << "Errors occurred during concurrent writing";

    // Verify all files exist and have correct content
    for (int id = 0; id < num_threads; ++id) {
        for (int i = 0; i < files_per_thread; ++i) {
            std::string fname = "file_" + std::to_string(id) + "_" + std::to_string(i);
            compio_file* f = compio_open_file(fname.c_str(), archive);
            ASSERT_NE(f, nullptr) << "Failed to open " << fname;

            std::vector<uint8_t> buffer(data_size);
            ASSERT_EQ(compio_read(buffer.data(), buffer.size(), f), data_size);

            // Verify content
            for (auto byte : buffer) {
                ASSERT_EQ(byte, (uint8_t)(id * 10 + i));
            }

            compio_close_file(f);
        }
    }
}

// Test 2: Cache Thrashing (LRU Thread Safety)
// Create many files, then read them randomly from multiple threads with a VERY small cache.
// This forces constant eviction and loading in the LRU cache, stressing its internal mutex.
TEST_F(AdvancedConcurrencyTest, CacheThrashing) {
    // Cache size 5 blocks -> very high churn
    create_archive(5);

    const int num_files = 100;
    const size_t file_size = 4096; // 1 block

    // 1. Create files sequentially (fast)
    {
        std::vector<uint8_t> data(file_size, 'A');
        for (int i = 0; i < num_files; ++i) {
            std::string fname = "f" + std::to_string(i);
            compio_file* f = compio_open_file(fname.c_str(), archive);
            compio_write(data.data(), file_size, f);
            compio_close_file(f);
        }
    }

    // 2. Concurrent Random Reads
    const int num_threads = 8;
    const int ops_per_thread = 500;
    std::atomic<int> errors{0};

    auto worker = [&](int id) {
        std::mt19937 rng(id + 999);
        std::vector<uint8_t> buffer(file_size);

        for (int i = 0; i < ops_per_thread; ++i) {
            int file_idx = rng() % num_files;
            std::string fname = "f" + std::to_string(file_idx);

            compio_file* f = compio_open_file(fname.c_str(), archive);
            if (!f) {
                errors++;
                continue;
            }

            // Read
            if (compio_read(buffer.data(), file_size, f) != file_size) {
                errors++;
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

    ASSERT_EQ(errors.load(), 0) << "Errors during cache thrashing test";
}

// Test 3: Read vs Write Exclusion
// Thread A reads continuously. Thread B writes continuously (new files).
// This verifies that SharedLock (Read) and UniqueLock (Write) interact correctly (no crashes).
TEST_F(AdvancedConcurrencyTest, ReadWhileWrite) {
    create_archive();

    // Create a large file for reading
    const std::string read_fname = "large_read_file";
    const size_t read_size = 1024 * 1024; // 1MB
    {
        compio_file* f = compio_open_file(read_fname.c_str(), archive);
        std::vector<uint8_t> data(read_size, 'R');
        compio_write(data.data(), read_size, f);
        compio_close_file(f);
    }

    std::atomic<bool> running{true};
    std::atomic<int> read_errors{0};
    std::atomic<int> write_errors{0};

    // Reader Thread
    std::thread reader([&]() {
        while (running) {
            compio_file* f = compio_open_file(read_fname.c_str(), archive);
            if (!f) {
                read_errors++;
                break;
            }
            
            std::vector<uint8_t> buf(read_size);
            if (compio_read(buf.data(), read_size, f) != read_size) {
                read_errors++;
            }
            compio_close_file(f);
        }
    });

    // Writer Thread
    std::thread writer([&]() {
        int i = 0;
        while (running) {
            std::string fname = "w" + std::to_string(i++);
            compio_file* f = compio_open_file(fname.c_str(), archive);
            if (!f) {
                write_errors++;
                break;
            }
            
            // Write small amount
            const char* msg = "hello";
            if (compio_write(msg, 5, f) != 5) {
                write_errors++;
            }
            compio_close_file(f);

            // Limit iterations to avoid disk filling
            if (i > 200) break;
        }
        running = false; // Stop reader when writer is done
    });

    writer.join();
    reader.join();

    ASSERT_EQ(read_errors.load(), 0);
    ASSERT_EQ(write_errors.load(), 0);
}

// Test 4: Defragmentation Safety
// Verifies that defragmentation is rejected if any file is open (even if idle).
// This ensures that we never move blocks while a reader might be holding a reference to them.
TEST_F(AdvancedConcurrencyTest, DefragSafetyCheck) {
    create_archive();

    // Create a file to read
    const std::string fname = "test_file";
    {
        compio_file* f = compio_open_file(fname.c_str(), archive);
        compio_write("data", 4, f);
        compio_close_file(f);
    }

    std::atomic<bool> file_opened{false};
    std::atomic<bool> can_close{false};

    std::thread reader([&]() {
        compio_file* f = compio_open_file(fname.c_str(), archive);
        if (f) {
            file_opened = true;
            // Hold file open until main thread signals
            while (!can_close) {
                std::this_thread::yield();
            }
            compio_close_file(f);
        }
    });

    // Wait for reader to open file
    while (!file_opened) {
        std::this_thread::yield();
    }
    
    // Attempt defrag while reader has file open
    // Should fail immediately because open_files_count > 0
    int res = compio_defragment(archive);
    EXPECT_EQ(res, COMPIO_ERROR) << "Defragmentation should fail when files are open";

    // Signal reader to close file
    can_close = true;
    reader.join();
    
    // After close, it should succeed
    res = compio_defragment(archive);
    EXPECT_EQ(res, COMPIO_SUCCESS) << "Defragmentation should succeed when no files are open";
}
