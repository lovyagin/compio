// Random Cantor set in R^n, generated one generation at a time through files.
//
// The cube [-1, 1]^n is split into m^n equal subcubes, each survives with probability p,
// and the step repeats inside every survivor. After k steps a random point is picked in
// every remaining cube. The expected dimension is ln(m^n * p) / ln(m). For large k the
// list of cubes does not fit into memory, so every generation is a logical file that is
// read sequentially while the next one is appended; the last one is replaced by the
// coordinates of the points. Earlier generations are deleted unless --keep=1.
//
// A cube of generation g is n uint32 indices in [0, m^g). Points are n doubles.
// All backends consume the same random stream in the same order and must produce the
// same points; the checksum printed at the end verifies it.
//
// Output is CSV on stdout; lines starting with '#' describe the run.
//
// Usage: cantor_set [--key=value ...]
//   --backend=NAME      stdio | ram | compio (default compio)
//   --compressor=NAME   lz4 | zlib | zstd | brotli | none, compio only (default none)
//   --wal=0|1           enable_wal, compio only (default 1)
//   --n=N --m=N         dimension and split factor (default 3, 4)
//   --p=X               survival probability of a subcube (default 0.1)
//   --k=N               steps (default 6)
//   --keep=0|1          keep intermediate generations (default 0)
//   --chunk=N           cubes per read/write call (default 4096)
//   --dir=PATH          directory for the files (default: system temp dir)
//   --seed=N            random seed (default 1)

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "compio.h"

namespace {

using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

struct splitmix64 {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
};

// Logical files with sequential append and offset reads, one implementation per candidate.
struct store {
    virtual ~store() = default;
    virtual void create(const std::string &name) = 0;
    virtual void append(const std::string &name, const void *data, size_t size) = 0;
    virtual size_t read(const std::string &name, uint64_t offset, void *buf, size_t size) = 0;
    virtual void remove(const std::string &name) = 0;
    virtual uint64_t size(const std::string &name) = 0;
    // Closes everything and returns the bytes the candidate occupies on disk.
    virtual uint64_t finish() = 0;
};

struct ram_store : store {
    std::map<std::string, std::vector<uint8_t>> files;
    void create(const std::string &name) override { files[name].clear(); }
    void append(const std::string &name, const void *data, size_t size) override {
        auto &v = files[name];
        const auto *p = static_cast<const uint8_t *>(data);
        v.insert(v.end(), p, p + size);
    }
    size_t read(const std::string &name, uint64_t offset, void *buf, size_t size) override {
        const auto &v = files[name];
        if (offset >= v.size()) return 0;
        const size_t got = std::min<size_t>(size, v.size() - offset);
        std::memcpy(buf, v.data() + offset, got);
        return got;
    }
    void remove(const std::string &name) override { files.erase(name); }
    uint64_t size(const std::string &name) override { return files[name].size(); }
    uint64_t finish() override { return 0; }
};

struct stdio_store : store {
    std::string dir;
    std::map<std::string, FILE *> files;

    explicit stdio_store(std::string d) : dir(std::move(d)) {}
    std::string path(const std::string &name) const { return dir + "/" + name; }
    uint64_t on_disk() const {
        uint64_t total = 0;
        for (const auto &[name, f] : files) {
            (void)f;
            std::error_code ec;
            const auto s = std::filesystem::file_size(path(name), ec);
            if (!ec) total += s;
        }
        return total;
    }
    void create(const std::string &name) override {
        FILE *f = std::fopen(path(name).c_str(), "w+b");
        if (!f) throw std::runtime_error("fopen " + name);
        files[name] = f;
    }
    void append(const std::string &name, const void *data, size_t size) override {
        FILE *f = files.at(name);
        fseeko(f, 0, SEEK_END);
        if (std::fwrite(data, 1, size, f) != size) throw std::runtime_error("fwrite " + name);
    }
    size_t read(const std::string &name, uint64_t offset, void *buf, size_t size) override {
        FILE *f = files.at(name);
        fseeko(f, static_cast<off_t>(offset), SEEK_SET);
        return std::fread(buf, 1, size, f);
    }
    void remove(const std::string &name) override {
        std::fclose(files.at(name));
        files.erase(name);
        std::remove(path(name).c_str());
    }
    uint64_t size(const std::string &name) override {
        FILE *f = files.at(name);
        std::fflush(f);
        fseeko(f, 0, SEEK_END);
        return static_cast<uint64_t>(ftello(f));
    }
    uint64_t finish() override {
        for (auto &[name, f] : files) std::fflush(f);
        const uint64_t total = on_disk();
        for (auto &[name, f] : files) {
            std::fclose(f);
            std::remove(path(name).c_str());
        }
        files.clear();
        return total;
    }
};

struct compio_store : store {
    std::string archive_path;
    compio_archive *archive = nullptr;
    std::map<std::string, compio_file *> files;

    compio_store(std::string path, const std::string &compressor, bool wal) : archive_path(std::move(path)) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.enable_wal = wal;
        if (compressor == "none") compio_build_dummy_compressor(&cfg.compressor);
        else if (compressor == "lz4") compio_build_compressor_by_type(&cfg.compressor, COMPIO_COMPRESS_LZ4);
        else if (compressor == "zlib") compio_build_compressor_by_type(&cfg.compressor, COMPIO_COMPRESS_ZLIB);
        else if (compressor == "zstd") compio_build_compressor_by_type(&cfg.compressor, COMPIO_COMPRESS_ZSTD);
        else if (compressor == "brotli") compio_build_compressor_by_type(&cfg.compressor, COMPIO_COMPRESS_BROTLI);
        else throw std::runtime_error("unknown compressor " + compressor);
        archive = compio_open_archive(archive_path.c_str(), "w", &cfg);
        if (!archive) throw std::runtime_error("compio_open_archive");
    }
    void create(const std::string &name) override {
        compio_file *f = compio_open_file(name.c_str(), archive);
        if (!f) throw std::runtime_error("compio_open_file " + name);
        files[name] = f;
    }
    void append(const std::string &name, const void *data, size_t size) override {
        compio_file *f = files.at(name);
        compio_seek(f, 0, COMPIO_SEEK_END);
        if (compio_write(data, size, f) != size) throw std::runtime_error("compio_write " + name);
    }
    size_t read(const std::string &name, uint64_t offset, void *buf, size_t size) override {
        compio_file *f = files.at(name);
        compio_seek(f, static_cast<int64_t>(offset), COMPIO_SEEK_SET);
        return compio_read(buf, size, f);
    }
    void remove(const std::string &name) override {
        compio_close_file(files.at(name));
        files.erase(name);
        if (compio_remove_file(archive, name.c_str()) != 0) throw std::runtime_error("compio_remove_file " + name);
    }
    uint64_t size(const std::string &name) override { return compio_get_size(files.at(name)); }
    uint64_t finish() override {
        for (auto &[name, f] : files) compio_close_file(f);
        files.clear();
        if (compio_close_archive(archive) != 0) throw std::runtime_error("compio_close_archive");
        archive = nullptr;
        std::error_code ec;
        const auto s = std::filesystem::file_size(archive_path, ec);
        std::filesystem::remove(archive_path, ec);
        std::filesystem::remove(archive_path + ".wal", ec);
        return ec ? 0 : s;
    }
};

struct options {
    std::string backend = "compio";
    std::string compressor = "none";
    std::string dir;
    uint32_t n = 3;
    uint32_t m = 4;
    double p = 0.1;
    uint32_t k = 6;
    bool keep = false;
    bool wal = true;
    size_t chunk = 4096;
    uint64_t seed = 1;
};

options parse(int argc, char **argv) {
    options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const auto eq = a.find('=');
        if (a.rfind("--", 0) != 0 || eq == std::string::npos) throw std::runtime_error("bad argument " + a);
        const std::string key = a.substr(2, eq - 2);
        const std::string v = a.substr(eq + 1);
        if (key == "backend") o.backend = v;
        else if (key == "compressor") o.compressor = v;
        else if (key == "dir") o.dir = v;
        else if (key == "n") o.n = static_cast<uint32_t>(std::stoul(v));
        else if (key == "m") o.m = static_cast<uint32_t>(std::stoul(v));
        else if (key == "p") o.p = std::stod(v);
        else if (key == "k") o.k = static_cast<uint32_t>(std::stoul(v));
        else if (key == "keep") o.keep = v != "0";
        else if (key == "wal") o.wal = v != "0";
        else if (key == "chunk") o.chunk = std::stoul(v);
        else if (key == "seed") o.seed = std::stoull(v);
        else throw std::runtime_error("unknown option " + key);
    }
    if (o.dir.empty()) o.dir = std::filesystem::temp_directory_path().string();
    if (o.n == 0 || o.m < 2 || o.k == 0 || o.p <= 0 || o.p > 1 || o.chunk == 0)
        throw std::runtime_error("invalid parameters");
    if (std::pow(static_cast<double>(o.m), o.k) >= 4294967296.0)
        throw std::runtime_error("m^k must fit into uint32");
    return o;
}

std::string gen_name(uint32_t g) { return "gen" + std::to_string(g); }

uint64_t read_chunks_and_expand(store &st, const options &o, uint32_t g, splitmix64 &rng, uint64_t threshold,
                                uint64_t subcubes, const std::vector<uint64_t> &powers) {
    const std::string in = gen_name(g);
    const std::string out = gen_name(g + 1);
    st.create(out);
    const size_t cube_words = o.n;
    const uint64_t total = st.size(in) / (cube_words * sizeof(uint32_t));
    std::vector<uint32_t> cubes(o.chunk * cube_words);
    std::vector<uint32_t> children;
    children.reserve(o.chunk * cube_words);
    uint64_t produced = 0;
    auto flush = [&] {
        if (children.empty()) return;
        st.append(out, children.data(), children.size() * sizeof(uint32_t));
        children.clear();
    };
    for (uint64_t done = 0; done < total;) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(o.chunk, total - done));
        const size_t bytes = take * cube_words * sizeof(uint32_t);
        if (st.read(in, done * cube_words * sizeof(uint32_t), cubes.data(), bytes) != bytes)
            throw std::runtime_error("short read of " + in);
        for (size_t c = 0; c < take; ++c) {
            const uint32_t *cube = &cubes[c * cube_words];
            for (uint64_t sub = 0; sub < subcubes; ++sub) {
                if (rng.next() >= threshold) continue;
                for (uint32_t d = 0; d < o.n; ++d) {
                    const uint32_t digit = static_cast<uint32_t>((sub / powers[d]) % o.m);
                    children.push_back(cube[d] * o.m + digit);
                }
                ++produced;
                if (children.size() >= o.chunk * cube_words) flush();
            }
        }
        done += take;
    }
    flush();
    return produced;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const options o = parse(argc, argv);
        const auto start = clock_type::now();

        std::unique_ptr<store> st;
        if (o.backend == "ram") {
            st = std::make_unique<ram_store>();
        } else if (o.backend == "stdio") {
            st = std::make_unique<stdio_store>(o.dir);
        } else if (o.backend == "compio") {
            st = std::make_unique<compio_store>(o.dir + "/cantor_" + std::to_string(::getpid()) + ".compio",
                                                o.compressor, o.wal);
        } else {
            throw std::runtime_error("unknown backend " + o.backend);
        }

        std::vector<uint64_t> powers(o.n, 1);
        for (uint32_t d = 1; d < o.n; ++d) powers[d] = powers[d - 1] * o.m;
        const uint64_t subcubes = powers[o.n - 1] * o.m;
        const uint64_t threshold =
            o.p >= 1.0 ? UINT64_MAX : static_cast<uint64_t>(static_cast<long double>(o.p) * 18446744073709551616.0L);
        splitmix64 rng{o.seed};

        std::printf("# backend=%s compressor=%s wal=%d n=%u m=%u p=%g k=%u keep=%d chunk=%zu seed=%" PRIu64 "\n",
                    o.backend.c_str(), o.backend == "compio" ? o.compressor.c_str() : "-", o.wal ? 1 : 0, o.n, o.m, o.p, o.k,
                    o.keep ? 1 : 0, o.chunk, o.seed);
        std::printf("generation,cubes,logical_bytes,seconds\n");

        const std::vector<uint32_t> origin(o.n, 0);
        st->create(gen_name(0));
        st->append(gen_name(0), origin.data(), origin.size() * sizeof(uint32_t));

        uint64_t last_cubes = 1;
        for (uint32_t g = 0; g < o.k; ++g) {
            const auto t = clock_type::now();
            last_cubes = read_chunks_and_expand(*st, o, g, rng, threshold, subcubes, powers);
            if (!o.keep) st->remove(gen_name(g));
            std::printf("%u,%" PRIu64 ",%" PRIu64 ",%.4f\n", g + 1, last_cubes, st->size(gen_name(g + 1)),
                        seconds_since(t));
        }

        // The last generation becomes the points: one random point in every cube.
        const auto t = clock_type::now();
        const std::string last = gen_name(o.k);
        st->create("points");
        const double side = 2.0 / std::pow(static_cast<double>(o.m), o.k);
        std::vector<uint32_t> cubes(o.chunk * o.n);
        std::vector<double> points;
        points.reserve(o.chunk * o.n);
        uint64_t checksum = 0xCBF29CE484222325ULL;
        for (uint64_t done = 0; done < last_cubes;) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(o.chunk, last_cubes - done));
            const size_t bytes = take * o.n * sizeof(uint32_t);
            if (st->read(last, done * o.n * sizeof(uint32_t), cubes.data(), bytes) != bytes)
                throw std::runtime_error("short read of " + last);
            points.clear();
            for (size_t i = 0; i < take * o.n; ++i) {
                const double x = -1.0 + (static_cast<double>(cubes[i]) + rng.unit()) * side;
                uint64_t bits;
                std::memcpy(&bits, &x, sizeof(bits));
                checksum = (checksum ^ bits) * 0x100000001B3ULL;
                points.push_back(x);
            }
            st->append("points", points.data(), points.size() * sizeof(double));
            done += take;
        }
        if (!o.keep) st->remove(last);
        std::printf("points,%" PRIu64 ",%" PRIu64 ",%.4f\n", last_cubes, st->size("points"), seconds_since(t));

        const uint64_t disk_bytes = st->finish();
        rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        const double expected = std::log(static_cast<double>(subcubes) * o.p) / std::log(static_cast<double>(o.m));
        const double measured = std::log(static_cast<double>(last_cubes)) / (o.k * std::log(static_cast<double>(o.m)));
        const auto seconds = [](const timeval &t) { return static_cast<double>(t.tv_sec) + t.tv_usec * 1e-6; };
        std::printf("# total_seconds=%.4f cpu_seconds=%.4f disk_bytes=%" PRIu64 " max_rss_kb=%ld\n",
                    seconds_since(start), seconds(usage.ru_utime) + seconds(usage.ru_stime), disk_bytes,
                    usage.ru_maxrss);
        std::printf("# dimension expected=%.4f measured=%.4f checksum=%016" PRIx64 "\n", expected, measured, checksum);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "cantor_set: %s\n", e.what());
        return 1;
    }
    return 0;
}
