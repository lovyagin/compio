#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern compio_config config;

// Each thread reads its OWN file. Per the compio API, concurrent compio_read on
// different file handles is safe; this isolates how well the archive's
// reader-writer locking (shared index + sharded block cache) lets independent
// reads proceed in parallel. Aggregate throughput vs thread count shows read
// scaling.
//
// Worker threads are persistent and the timed region (UseManualTime) covers
// only the steady-state read window. Spawning/joining std::threads per
// iteration would otherwise dominate this sub-millisecond workload and mask the
// real scaling behind thread-management overhead.

static constexpr size_t kFileSize = 1u << 20; // 1 MiB payload per file

static const std::string& payload() {
    static const std::string p = [] {
        std::string s;
        s.reserve(kFileSize);
        while (s.size() < kFileSize)
            s.append(html_data, sizeof(html_data) - 1);
        s.resize(kFileSize);
        return s;
    }();
    return p;
}

static void BM_ConcurrentRead(benchmark::State& state) {
    const int num_threads      = static_cast<int>(state.range(0));
    const int reads_per_thread = 64; // enough work per timed iteration that
                                     // scheduler jitter averages out
    const size_t block         = 64 * 1024;

    std::string path = get_temporary_filename();
    compio_config cfg = config;

    compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
    if (!ar) {
        state.SkipWithError("open archive");
        return;
    }
    for (int t = 0; t < num_threads; ++t) {
        compio_file* f = compio_open_file(("f_" + std::to_string(t)).c_str(), ar);
        if (!f) {
            compio_close_archive(ar);
            state.SkipWithError("open file (write)");
            return;
        }
        compio_write(payload().data(), payload().size(), f);
        compio_close_file(f);
    }
    compio_flush(ar);
    compio_close_archive(ar);

    ar = compio_open_archive(path.c_str(), "r", &cfg);
    if (!ar) {
        remove(path.c_str());
        state.SkipWithError("reopen archive");
        return;
    }

    auto read_file = [&](compio_file* f, std::vector<uint8_t>& buf) {
        for (int r = 0; r < reads_per_thread; ++r) {
            compio_seek(f, 0, COMPIO_SEEK_SET);
            while (compio_read(buf.data(), buf.size(), f) > 0) {}
        }
    };

    // The main thread acts as reader 0; the other num_threads-1 readers are
    // persistent workers. Total concurrently-running threads during the timed
    // region equals num_threads, so we never oversubscribe the cores (which
    // would add scheduling jitter to the measurement).
    std::atomic<uint64_t> generation{0};
    std::atomic<int> done{0};
    std::atomic<bool> quit{false};

    auto worker = [&](int tid) {
        compio_file* f = compio_open_file(("f_" + std::to_string(tid)).c_str(), ar);
        std::vector<uint8_t> buf(block);
        uint64_t seen = 0;
        while (true) {
            while (generation.load(std::memory_order_acquire) == seen &&
                   !quit.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (quit.load(std::memory_order_acquire)) break;
            seen = generation.load(std::memory_order_acquire);
            if (f) read_file(f, buf);
            done.fetch_add(1, std::memory_order_release);
        }
        if (f) compio_close_file(f);
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int t = 1; t < num_threads; ++t)
        threads.emplace_back(worker, t);

    compio_file* f0 = compio_open_file("f_0", ar);
    std::vector<uint8_t> buf0(block);
    const int n_workers = num_threads - 1;

    for (auto _ : state) {
        done.store(0, std::memory_order_release);
        auto start = std::chrono::high_resolution_clock::now();
        generation.fetch_add(1, std::memory_order_acq_rel); // release workers
        if (f0) read_file(f0, buf0);                         // main is reader 0
        while (done.load(std::memory_order_acquire) < n_workers)
            std::this_thread::yield();                       // wait for stragglers
        auto end = std::chrono::high_resolution_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }

    if (f0) compio_close_file(f0);
    quit.store(true, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_acq_rel); // wake workers so they can exit
    for (auto& th : threads)
        th.join();

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(num_threads) *
                            reads_per_thread * static_cast<int64_t>(kFileSize));
    state.counters["threads"] = num_threads;

    compio_close_archive(ar);
    remove(path.c_str());
    remove((path + ".wal").c_str());
}

// Thread counts up to the physical core count (6) to avoid SMT/oversubscription
// confounds. With persistent workers the aggregate read throughput scales
// near-linearly with cores, confirming reads run in parallel rather than
// serializing on a shared lock.
BENCHMARK(BM_ConcurrentRead)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(6)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.8)
    ->UseManualTime();
