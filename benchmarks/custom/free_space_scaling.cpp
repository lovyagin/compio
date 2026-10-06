// Cost of the free space manager as the number of free regions grows.
//
// Works on free_blocks_manager alone (no file I/O, no compression): builds F
// isolated free regions, then repeatedly allocates a region and gives it back,
// so F stays constant. Reported per strategy: time to build the set (what an
// archive open pays to load the saved state), one allocation, one release.
//
// Usage: free_space_scaling [max_regions]   (default 256000)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "compio/allocator.hpp"

using compio::allocation_strategy;
using compio::free_blocks_manager;

static double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
    const uint64_t max_regions = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 256000;
    const char *names[] = {"first", "best", "worst", "next"};
    constexpr uint64_t SLOT = 32768;
    constexpr int ITERATIONS = 20000;

    std::printf("regions,strategy,build_s,allocate_ns,release_ns\n");
    for (uint64_t regions = 1000; regions <= max_regions; regions *= 4) {
        for (int s = 0; s < 4; ++s) {
            uint64_t file_size = regions * SLOT + (1 << 20);
            free_blocks_manager manager(&file_size);
            std::mt19937_64 rng(7);

            const double build_start = seconds_now();
            for (uint64_t i = 0; i < regions; ++i) {
                manager.add_free_block(1024 + i * SLOT, 64 + rng() % 16321);
            }
            const double build_time = seconds_now() - build_start;

            double allocate_time = 0;
            double release_time = 0;
            for (int i = 0; i < ITERATIONS; ++i) {
                const uint64_t size = 64 + rng() % 8129;
                double t = seconds_now();
                const uint64_t offset = manager.allocate_block(size, static_cast<allocation_strategy>(s));
                allocate_time += seconds_now() - t;
                if (offset == UINT64_MAX) continue;
                t = seconds_now();
                manager.add_free_block(offset, size);
                release_time += seconds_now() - t;
            }

            std::printf("%llu,%s,%.4f,%.0f,%.0f\n", static_cast<unsigned long long>(regions), names[s],
                        build_time, allocate_time / ITERATIONS * 1e9, release_time / ITERATIONS * 1e9);
            std::fflush(stdout);
        }
    }
    return 0;
}
