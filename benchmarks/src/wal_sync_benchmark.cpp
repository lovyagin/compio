#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

#include <benchmark/benchmark.h>
#include <string>

extern compio_config config;

// Same write workload under each WAL sync mode, exposing the cost of durability:
//   ALWAYS  fsyncs after every committed op (strict crash safety),
//   NORMAL  defers fsync to checkpoint/flush/close,
//   OFF     never fsyncs the WAL on commit (journal still written, just not synced),
//   NO-WAL  journaling disabled entirely (no double-write) -> cost of journaling itself.
// Comparing OFF vs NO-WAL isolates the price of maintaining the journal from the
// price of fsync (ALWAYS vs the rest).

static void BM_WalSyncWrite(benchmark::State& state) {
    const int mode     = static_cast<int>(state.range(0));
    const int n_ops    = 256;
    const size_t block = 4096;
    const size_t span  = sizeof(html_data) - 1 - block;

    compio_config cfg = config;
    if (mode == 3) {
        cfg.enable_wal = false;
    } else {
        cfg.wal_sync_mode = static_cast<compio_wal_sync_mode>(mode);
    }

    for (auto _ : state) {
        std::string path = get_temporary_filename();
        compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
        if (!ar) {
            state.SkipWithError("open archive");
            break;
        }
        compio_file* f = compio_open_file("A", ar);
        if (!f) {
            compio_close_archive(ar);
            state.SkipWithError("open file");
            break;
        }
        for (int i = 0; i < n_ops; ++i)
            compio_write(html_data + (static_cast<size_t>(i) * 131) % span, block, f);
        compio_close_file(f);
        compio_close_archive(ar);
        remove(path.c_str());
        remove((path + ".wal").c_str());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(n_ops) * block);
    state.counters["sync_mode"] = mode; // 0=ALWAYS 1=NORMAL 2=OFF 3=NO-WAL
}

BENCHMARK(BM_WalSyncWrite)
    ->Arg(0)
    ->Arg(1)
    ->Arg(2)
    ->Arg(3)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.5)
    ->UseRealTime();
