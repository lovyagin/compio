// Sustained random modification of one logical file.
//
// Fills a file, then applies random overwrites / inserts / erases and reports,
// per window of operations, the throughput and the state of the container, so
// that degradation (or its absence) shows up as a trend over the run. Sequential
// and random read speed are measured before and after the modifications.
//
// Output is CSV on stdout; lines starting with '#' describe the run.
//
// Usage: random_modification [--key=value ...]
//   --dir=PATH          directory for the archive (default: system temp dir)
//   --data=PATH         file to take the payload from (default: synthetic text)
//   --file-mb=N         size of the logical file, MiB (default 64)
//   --ops=N             number of modifications (default 50000)
//   --window=N          operations per reported row (default 5000)
//   --size-mean=N       mean size of one modification, bytes (default 8192)
//   --size-sigma=N      standard deviation of that size (default 2048)
//   --overwrite=N --insert=N --erase=N   operation mix, weights (default 100 0 0)
//   --random-share=N    percent of modifications that write incompressible data (default 33)
//   --compressor=NAME   lz4 | zlib | zstd | brotli | none (default lz4)
//   --strategy=NAME     first | best | worst | next (default: library default)
//   --threshold=N       fragmentation_threshold, 100 disables compaction (default 100)
//   --punch-holes=0|1   fill_holes_with_zeros (default 0)
//   --block=N --block-min=N --block-max=N   block sizes (default: library defaults)
//   --degree=N          B-tree degree (default: library default)
//   --cache-blocks=N    cache_size__blocks (default: library default)
//   --wal=0|1           enable_wal (default 1)
//   --seed=N            random seed (default 1)

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "compio.h"

namespace {

using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

struct file_usage {
    uint64_t size;      // st_size
    uint64_t allocated; // st_blocks * 512: differs from size for a sparse file
};

file_usage usage_of(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return {0, 0};
    }
#ifdef _WIN32
    return {static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_size)};
#else
    return {static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_blocks) * 512};
#endif
}

std::vector<uint8_t> synthetic_text(size_t size, uint64_t seed) {
    static const char *const words[] = {"alpha", "beta",  "gamma",  "delta", "epsilon",
                                        "zeta",  "eta",   "theta",  "iota",  "kappa",
                                        "lambda", "mu",   "nu",     "xi",    "omicron"};
    std::mt19937_64 rng(seed);
    std::vector<uint8_t> out;
    out.reserve(size + 16);
    while (out.size() < size) {
        const char *word = words[rng() % 15];
        out.insert(out.end(), word, word + strlen(word));
        out.push_back(rng() % 7 ? ' ' : static_cast<uint8_t>('0' + rng() % 10));
    }
    out.resize(size);
    return out;
}

std::vector<uint8_t> random_bytes(size_t size, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint8_t> out(size);
    for (auto &b : out) b = static_cast<uint8_t>(rng());
    return out;
}

std::vector<uint8_t> load_file(const std::string &path, size_t limit) {
    std::vector<uint8_t> out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    out.resize(limit);
    out.resize(fread(out.data(), 1, limit, f));
    fclose(f);
    return out;
}

struct options {
    std::map<std::string, std::string> values;

    options(int argc, char **argv) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            const size_t eq = arg.find('=');
            if (arg.rfind("--", 0) != 0 || eq == std::string::npos) {
                fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
                exit(2);
            }
            values[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
        }
    }

    bool has(const std::string &key) const { return values.count(key) != 0; }
    std::string text(const std::string &key, const std::string &fallback) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    int64_t number(const std::string &key, int64_t fallback) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : strtoll(it->second.c_str(), nullptr, 10);
    }
};

bool select_compressor(const std::string &name, compio_compressor *out) {
    if (name == "lz4") compio_build_lz4_compressor(out);
    else if (name == "zlib") compio_build_zlib_compressor(out);
    else if (name == "zstd") compio_build_zstd_compressor(out);
    else if (name == "brotli") compio_build_brotli_compressor(out);
    else if (name == "none") compio_build_dummy_compressor(out);
    else return false;
    return true;
}

bool select_strategy(const std::string &name, compio_allocation_strategy *out) {
    if (name == "first") *out = COMPIO_ALLOC_FIRST_FIT;
    else if (name == "best") *out = COMPIO_ALLOC_BEST_FIT;
    else if (name == "worst") *out = COMPIO_ALLOC_WORST_FIT;
    else if (name == "next") *out = COMPIO_ALLOC_NEXT_FIT;
    else return false;
    return true;
}

const char *strategy_name(compio_allocation_strategy s) {
    switch (s) {
    case COMPIO_ALLOC_BEST_FIT: return "best";
    case COMPIO_ALLOC_WORST_FIT: return "worst";
    case COMPIO_ALLOC_NEXT_FIT: return "next";
    default: return "first";
    }
}

// Speed of reading the whole file front to back, MiB/s.
double sequential_read_speed(compio_file *file) {
    std::vector<uint8_t> buffer(1 << 20);
    const uint64_t size = compio_get_size(file);
    compio_seek(file, 0, COMPIO_SEEK_SET);
    const auto start = clock_type::now();
    uint64_t done = 0;
    while (done < size) {
        const uint64_t n = compio_read(buffer.data(), buffer.size(), file);
        if (n == 0) break;
        done += n;
    }
    return static_cast<double>(done) / (1 << 20) / seconds_since(start);
}

// Speed of reads of read_size bytes at random positions, operations per second.
double random_read_speed(compio_file *file, uint64_t read_size, int count, uint64_t seed) {
    std::vector<uint8_t> buffer(read_size);
    const uint64_t size = compio_get_size(file);
    if (size <= read_size) return 0.0;
    std::mt19937_64 rng(seed);
    const auto start = clock_type::now();
    for (int i = 0; i < count; i++) {
        compio_seek(file, static_cast<int64_t>(rng() % (size - read_size)), COMPIO_SEEK_SET);
        compio_read(buffer.data(), read_size, file);
    }
    return count / seconds_since(start);
}

// Both measurements start cold: the archive is synchronised and then read
// through a separate read-only handle, which has caches of its own.
void report_read_speed(const char *label, compio_archive *archive, const std::string &path,
                       const compio_config &cfg, uint64_t seed) {
    compio_flush(archive);

    double speeds[2] = {0.0, 0.0};
    for (int kind = 0; kind < 2; kind++) {
        compio_archive *reader = compio_open_archive(path.c_str(), "r", &cfg);
        compio_file *file = reader ? compio_open_file("data", reader) : nullptr;
        if (file) {
            speeds[kind] = kind == 0 ? sequential_read_speed(file) : random_read_speed(file, 4096, 20000, seed);
            compio_close_file(file);
        }
        if (reader) compio_close_archive(reader);
    }
    printf("# %s: sequential read %.1f MiB/s, random 4 KiB read %.0f ops/s\n", label, speeds[0], speeds[1]);
}

} // namespace

int main(int argc, char **argv) {
    const options opt(argc, argv);

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    if (!select_compressor(opt.text("compressor", "lz4"), &cfg.compressor)) {
        fprintf(stderr, "unknown compressor\n");
        return 2;
    }
    if (opt.has("strategy") && !select_strategy(opt.text("strategy", ""), &cfg.allocation_strategy)) {
        fprintf(stderr, "unknown strategy\n");
        return 2;
    }
    cfg.fragmentation_threshold = static_cast<uint8_t>(opt.number("threshold", 100));
    cfg.fill_holes_with_zeros = opt.number("punch-holes", 0) != 0;
    cfg.block_size = static_cast<int>(opt.number("block", cfg.block_size));
    cfg.block_size__minimum = static_cast<int>(opt.number("block-min", cfg.block_size__minimum));
    cfg.block_size__maximum = static_cast<int>(opt.number("block-max", cfg.block_size__maximum));
    cfg.b_tree_degree = static_cast<int>(opt.number("degree", cfg.b_tree_degree));
    cfg.cache_size__blocks = static_cast<int>(opt.number("cache-blocks", cfg.cache_size__blocks));
    cfg.enable_wal = opt.number("wal", 1) != 0;

    const uint64_t file_size = static_cast<uint64_t>(opt.number("file-mb", 64)) << 20;
    const int64_t total_ops = opt.number("ops", 50000);
    const int64_t window = std::max<int64_t>(1, opt.number("window", 5000));
    const double size_mean = static_cast<double>(opt.number("size-mean", 8192));
    const double size_sigma = static_cast<double>(opt.number("size-sigma", 2048));
    const int64_t w_overwrite = opt.number("overwrite", 100);
    const int64_t w_insert = opt.number("insert", 0);
    const int64_t w_erase = opt.number("erase", 0);
    const int64_t w_total = w_overwrite + w_insert + w_erase;
    const int64_t random_share = opt.number("random-share", 33);
    const uint64_t seed = static_cast<uint64_t>(opt.number("seed", 1));
    if (w_total <= 0 || file_size == 0) {
        fprintf(stderr, "empty operation mix or file\n");
        return 2;
    }

    const size_t pool_size = 32u << 20;
    std::vector<uint8_t> text = opt.has("data") ? load_file(opt.text("data", ""), pool_size)
                                                : synthetic_text(pool_size, seed);
    if (text.size() < (1u << 20)) {
        fprintf(stderr, "payload is missing or shorter than 1 MiB\n");
        return 2;
    }
    const std::vector<uint8_t> noise = random_bytes(text.size(), seed + 1);
    const uint64_t max_op_size = std::min<uint64_t>(text.size() / 2, static_cast<uint64_t>(size_mean + 8 * size_sigma));

    const std::string dir = opt.text("dir", std::filesystem::temp_directory_path().string());
    const std::string path = dir + "/random_modification.compio";
    remove(path.c_str());
    remove((path + ".wal").c_str());

    compio_archive *archive = compio_open_archive(path.c_str(), "w", &cfg);
    if (!archive) {
        fprintf(stderr, "cannot create %s\n", path.c_str());
        return 1;
    }
    compio_file *file = compio_open_file("data", archive);

    const auto fill_start = clock_type::now();
    for (uint64_t done = 0; done < file_size;) {
        const uint64_t n = std::min<uint64_t>(text.size(), file_size - done);
        compio_write(text.data(), n, file);
        done += n;
    }
    compio_flush(archive);
    const double fill_speed = static_cast<double>(file_size) / (1 << 20) / seconds_since(fill_start);
    const file_usage after_fill = usage_of(path);

    printf("# compressor=%s strategy=%s threshold=%d punch_holes=%d block=%d/%d/%d degree=%d "
           "cache_blocks=%d wal=%d\n",
           opt.text("compressor", "lz4").c_str(), strategy_name(cfg.allocation_strategy),
           cfg.fragmentation_threshold, cfg.fill_holes_with_zeros, cfg.block_size__minimum, cfg.block_size,
           cfg.block_size__maximum, cfg.b_tree_degree, cfg.cache_size__blocks, cfg.enable_wal);
    printf("# file_mb=%llu ops=%lld size=%.0f+-%.0f mix=%lld/%lld/%lld random_share=%lld seed=%llu\n",
           static_cast<unsigned long long>(file_size >> 20), static_cast<long long>(total_ops), size_mean,
           size_sigma, static_cast<long long>(w_overwrite), static_cast<long long>(w_insert),
           static_cast<long long>(w_erase), static_cast<long long>(random_share),
           static_cast<unsigned long long>(seed));
    printf("# fill: %.1f MiB/s, container %llu bytes, ratio %.4f\n", fill_speed,
           static_cast<unsigned long long>(after_fill.size), static_cast<double>(after_fill.size) / file_size);
    report_read_speed("before", archive, path, cfg, seed + 2);

    printf("ops,ops_per_s,mib_per_s,logical_bytes,container_bytes,disk_bytes,free_bytes,free_regions,"
           "fragmentation_percent,container_ratio,disk_ratio\n");

    std::mt19937_64 rng(seed + 3);
    std::normal_distribution<double> size_dist(size_mean, size_sigma);
    auto window_start = clock_type::now();
    uint64_t window_bytes = 0;

    for (int64_t i = 1; i <= total_ops; i++) {
        const uint64_t logical = compio_get_size(file);
        uint64_t size = static_cast<uint64_t>(std::max(1.0, size_dist(rng)));
        size = std::min(size, max_op_size);
        const int64_t pick = static_cast<int64_t>(rng() % static_cast<uint64_t>(w_total));
        const std::vector<uint8_t> &source =
            (static_cast<int64_t>(rng() % 100) < random_share) ? noise : text;
        const uint8_t *payload = source.data() + rng() % (source.size() - size);

        if (pick < w_overwrite) {
            size = std::min(size, logical);
            compio_seek(file, static_cast<int64_t>(rng() % (logical - size + 1)), COMPIO_SEEK_SET);
            compio_write(payload, size, file);
        } else if (pick < w_overwrite + w_insert) {
            compio_seek(file, static_cast<int64_t>(rng() % (logical + 1)), COMPIO_SEEK_SET);
            compio_insert(payload, size, file);
        } else {
            size = std::min(size, logical / 2);
            compio_seek(file, static_cast<int64_t>(rng() % (logical - size + 1)), COMPIO_SEEK_SET);
            compio_erase(size, file);
        }
        window_bytes += size;

        if (i % window == 0 || i == total_ops) {
            const double elapsed = seconds_since(window_start);
            const int64_t ops_in_window = (i % window == 0) ? window : i % window;
            compio_fragmentation_stats stats{};
            compio_get_fragmentation_stats(archive, &stats);
            const file_usage usage = usage_of(path);
            const uint64_t logical_now = compio_get_size(file);
            printf("%lld,%.0f,%.2f,%llu,%llu,%llu,%zu,%zu,%u,%.4f,%.4f\n", static_cast<long long>(i),
                   ops_in_window / elapsed, static_cast<double>(window_bytes) / (1 << 20) / elapsed,
                   static_cast<unsigned long long>(logical_now),
                   static_cast<unsigned long long>(usage.size),
                   static_cast<unsigned long long>(usage.allocated), stats.total_free_bytes,
                   stats.num_free_regions, stats.fragmentation_percent,
                   static_cast<double>(usage.size) / logical_now,
                   static_cast<double>(usage.allocated) / logical_now);
            fflush(stdout);
            window_start = clock_type::now();
            window_bytes = 0;
        }
    }

    report_read_speed("after", archive, path, cfg, seed + 2);

    const uint64_t final_logical = compio_get_size(file);
    const auto close_start = clock_type::now();
    compio_close_file(file);
    compio_close_archive(archive);
    const file_usage closed = usage_of(path);
    printf("# close: %.2f s, container %llu bytes (ratio %.4f), on disk %llu bytes (ratio %.4f)\n",
           seconds_since(close_start), static_cast<unsigned long long>(closed.size),
           static_cast<double>(closed.size) / final_logical,
           static_cast<unsigned long long>(closed.allocated),
           static_cast<double>(closed.allocated) / final_logical);

    remove(path.c_str());
    remove((path + ".wal").c_str());
    return 0;
}
