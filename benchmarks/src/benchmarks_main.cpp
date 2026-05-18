#include <benchmark/benchmark.h>
#include <memory>

#include "benchmark_util.hpp"

#include "compio.h"

compio_config config;

std::string get_config_fn(int argc, char **argv) {
    const std::string prefix("--compio_config=");
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!arg.compare(0, prefix.size(), prefix)) {
            return arg.substr(prefix.size());
        }
    }
    return "";
}

void build_config(int argc, char **argv) {
    compio_build_default_config(&config);
    // Use relaxed durability for benchmarks to match stdio buffering behavior
    // (stdio buffers in memory and doesn't fsync on every write)
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    
    std::string config_fn = get_config_fn(argc, argv);
    if (config_fn.size() > 0) {
        auto bc = build_config_from_file(config_fn, &config);
        for (const auto &[key, val] : bc) {
            benchmark::AddCustomContext(key, val);
        }
    }
}

int main(int argc, char **argv) {
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    build_config(argc, argv);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
