#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <string>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "compio.h"
#include "test_util.hpp"

class ConcurrencyStressTest : public ::testing::Test {
protected:
    char filename[256];
    compio_archive* archive;
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));
        compio_build_default_config(&config);
        config.wal_max_size_bytes = 10 * 1024 * 1024; 
        
        archive = compio_open_archive(filename, "w+", &config);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) {
            compio_close_archive(archive);
        }
        std::remove(filename);
    }
};

TEST_F(ConcurrencyStressTest, ParallelUniqueFileOperations) {
    const int num_threads = 8;
    const int ops_per_thread = 10;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, i, &errors]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                std::string fname = "file_" + std::to_string(i) + "_" + std::to_string(j);
                std::string content = "data_" + fname;
                
                // Create/Write
                compio_file* f = compio_open_file(fname.c_str(), archive);
                if (!f) {
                    errors++;
                    continue;
                }
                
                if (compio_write(content.data(), content.size(), f) != content.size()) {
                    errors++;
                    compio_close_file(f);
                    continue;
                }
                compio_close_file(f);

                // Read and verify
                f = compio_open_file(fname.c_str(), archive);
                if (!f) {
                    errors++;
                    continue;
                }

                size_t fsize = compio_get_size(f);
                if (fsize != content.size()) {
                    errors++;
                    compio_close_file(f);
                    continue;
                }

                std::vector<uint8_t> buffer(fsize);
                if (compio_read(buffer.data(), fsize, f) != fsize) {
                    errors++;
                    compio_close_file(f);
                    continue;
                }
                compio_close_file(f);

                std::string read_content(buffer.begin(), buffer.end());
                if (read_content != content) {
                    errors++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0) << "Errors occurred during parallel operations";
}

TEST_F(ConcurrencyStressTest, ParallelReadSharedWriteUnique) {
    // One writer, multiple readers
    const int num_readers = 4;
    const int initial_files = 5;
    
    // Pre-populate
    for(int i=0; i<initial_files; ++i) {
        std::string fname = "shared_" + std::to_string(i);
        compio_file* f = compio_open_file(fname.c_str(), archive);
        ASSERT_NE(f, nullptr);
        std::string content = "content_" + std::to_string(i);
        compio_write(content.data(), content.size(), f);
        compio_close_file(f);
    }

    std::atomic<bool> run{true};
    std::atomic<int> read_errors{0};

    // Readers
    std::vector<std::thread> readers;
    for(int i=0; i<num_readers; ++i) {
        readers.emplace_back([this, &run, &read_errors]() {
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, initial_files - 1);
            
            while(run.load()) {
                int idx = dist(rng);
                std::string fname = "shared_" + std::to_string(idx);
                std::string expected = "content_" + std::to_string(idx);
                
                compio_file* f = compio_open_file(fname.c_str(), archive);
                if (!f) {
                     read_errors++;
                     continue;
                }

                std::vector<uint8_t> buf(expected.size());
                if (compio_read(buf.data(), buf.size(), f) != buf.size()) {
                     read_errors++;
                } else {
                     std::string actual(buf.begin(), buf.end());
                     if(actual != expected) {
                         read_errors++;
                     }
                }
                compio_close_file(f);
            }
        });
    }

    // Writer (creates new files and deletes some new files)
    std::thread writer([this, &run, &read_errors]() {
        for(int i=0; i<10; ++i) {
            std::string fname = "new_" + std::to_string(i);
            compio_file* f = compio_open_file(fname.c_str(), archive);
            if (!f) {
                read_errors++; // piggyback on read_errors
                continue;
            }
            
            std::string content = "new_data_" + std::to_string(i);
            if (compio_write(content.data(), content.size(), f) != content.size()) {
                read_errors++;
            }
            compio_close_file(f);

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            
            if (i > 2) {
                 // Delete an old 'new' file
                 std::string del_fname = "new_" + std::to_string(i - 2);
                 compio_remove_file(archive, del_fname.c_str());
            }
        }
        run.store(false);
    });

    writer.join();
    for(auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(read_errors.load(), 0) << "Errors occurred during read/write mix";
}
