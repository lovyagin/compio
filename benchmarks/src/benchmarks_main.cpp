#include <benchmark/benchmark.h>

#include "compio.h"
#include "benchmark_util.hpp"

#include <memory>

compio_config config;

std::string get_config_fn(int argc, char** argv) {
    const std::string prefix("--compio_config=");
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!arg.compare(0, prefix.size(), prefix)) {
            return arg.substr(prefix.size());
        }
    }
    return "";
}

void build_config(int argc, char** argv) {
    compio_build_default_config(&config);
    std::string config_fn = get_config_fn(argc, argv);
    if (config_fn.size() > 0) {
        auto bc = build_config_from_file(config_fn, &config);
        for (const auto& [key, val] : bc) {
            benchmark::AddCustomContext(key, val);
        }
    }
}

int main(int argc, char** argv) {
    build_config(argc, argv);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}