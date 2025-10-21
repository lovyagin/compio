/**
 * @file cpp_wrapper_example.cpp
 * @brief Example demonstrating C++ wrapper usage with stream-style I/O
 */

#include "../compio.hpp"
#include <cstring>
#include <iostream>
#include <vector>

struct Point {
    double x;
    double y;
    double z;
};

struct ComplexData {
    int32_t id;
    float value;
    uint64_t timestamp;
};

int main() {
    try {
        // Create archive with custom configuration
        compio::Config config;
        config.set_block_size(4096)
              .set_btree_degree(5)
              .set_compressor(compio::compressors::lz4())
              .set_cache_blocks(100);

        // Open archive using RAII - automatically closes on scope exit
        compio::Archive archive("test_cpp.compio", "w+", config);

        // Example 1: Binary I/O with basic types
        {
            auto file = archive.open_file("numbers.bin");

            // Write using stream operators
            int32_t a = 42;
            double b = 3.14159;
            uint64_t c = 0xDEADBEEF;

            file << a << b << c;

            std::cout << "Written: " << a << ", " << b << ", " << c << std::endl;
        }

        // Example 2: Reading back data
        {
            auto file = archive.open_file("numbers.bin");

            int32_t a;
            double b;
            uint64_t c;

            file >> a >> b >> c;

            std::cout << "Read: " << a << ", " << b << ", " << c << std::endl;
        }

        // Example 3: Writing structures
        {
            auto file = archive.open_file("points.bin");

            std::vector<Point> points = {
                {1.0, 2.0, 3.0},
                {4.0, 5.0, 6.0},
                {7.0, 8.0, 9.0}
            };

            // Write number of points
            uint32_t count = points.size();
            file << count;

            // Write all points
            for (const auto& p : points) {
                file << p;
            }

            std::cout << "Written " << count << " points" << std::endl;
        }

        // Example 4: Reading structures
        {
            auto file = archive.open_file("points.bin");

            uint32_t count;
            file >> count;

            std::vector<Point> points(count);
            for (auto& p : points) {
                file >> p;
            }

            std::cout << "Read " << count << " points:" << std::endl;
            for (const auto& p : points) {
                std::cout << "  (" << p.x << ", " << p.y << ", " << p.z << ")" << std::endl;
            }
        }

        // Example 5: Seeking and random access
        {
            auto file = archive.open_file("data.bin");

            // Write array of complex data
            std::vector<ComplexData> data = {
                {1, 1.1f, 1000},
                {2, 2.2f, 2000},
                {3, 3.3f, 3000},
                {4, 4.4f, 4000}
            };

            for (const auto& d : data) {
                file << d;
            }

            // Seek to third element
            file.seek(2 * sizeof(ComplexData), COMP_SEEK_SET);

            ComplexData third;
            file >> third;

            std::cout << "Third element: id=" << third.id
                      << ", value=" << third.value
                      << ", timestamp=" << third.timestamp << std::endl;
        }

        // Example 6: Using raw read/write
        {
            auto file = archive.open_file("raw.bin");

            const char* text = "Hello, compio C++ wrapper!";
            file.write(text, strlen(text));

            file.seek(0, COMP_SEEK_SET);

            char buffer[100] = {0};
            file.read(buffer, strlen(text));

            std::cout << "Raw data: " << buffer << std::endl;
        }

        // Flush changes to disk
        archive.flush();

        std::cout << "All operations completed successfully!" << std::endl;

    } catch (const compio::Exception& e) {
        std::cerr << "Compio error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
