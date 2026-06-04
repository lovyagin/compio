#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

#include <benchmark/benchmark.h>
#include <string>
#include <thread>
#include <vector>

extern compio_config config;

// Each thread reads its OWN file. Per the compio API, concurrent compio_read on
// different file handles is safe; this isolates how well the archive's
// reader-writer locking (shared index + block cache) lets independent reads
// proceed in parallel. Aggregate throughput vs thread count shows read scaling.

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
    const int reads_per_thread = 16;
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

    auto worker = [&](int tid) {
        compio_file* f = compio_open_file(("f_" + std::to_string(tid)).c_str(), ar);
        if (!f) return;
        std::vector<uint8_t> buf(block);
        for (int r = 0; r < reads_per_thread; ++r) {
            compio_seek(f, 0, COMPIO_SEEK_SET);
            while (compio_read(buf.data(), buf.size(), f) > 0) {}
        }
        compio_close_file(f);
    };

    for (auto _ : state) {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
            threads.emplace_back(worker, t);
        for (auto& th : threads)
            th.join();
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(num_threads) *
                            reads_per_thread * static_cast<int64_t>(kFileSize));
    state.counters["threads"] = num_threads;

    compio_close_archive(ar);
    remove(path.c_str());
    remove((path + ".wal").c_str());
}

// Thread counts up to the physical core count (6) to avoid SMT/oversubscription
// confounds: the read path is memory-bandwidth bound once cores saturate.
BENCHMARK(BM_ConcurrentRead)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(6)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.8)
    ->UseRealTime();
