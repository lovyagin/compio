// Index corruption check.
//
// Builds a small valid archive, then damages one B-tree node in a copy of it
// and reads every file back in a child process (so that a hang or a crash of
// the library is an outcome, not the end of the run). Three kinds of damage:
//   bytes  a few random bytes inside the node
//   stale  the node is replaced by the content of another node, as after a
//          misdirected write or a write that never reached the disk
//   torn   the tail of the node is zeroed, as after an interrupted write
// The library should answer every such archive with an error. Returning data
// that differs from what was written, crashing and hanging are failures.
//
// Usage: index_corruption [--key=value ...]
//   --dir=PATH    directory for the archives (default: system temp dir)
//   --trials=N    damaged archives per kind of damage (default 200)
//   --seed=N      random seed (default 1)
//
// Exit code is 0 if no trial returned wrong data, crashed or hung.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/file.hpp"

namespace {

enum class outcome { unaffected, error_reported, wrong_data, crashed, hung };
constexpr int OUTCOMES = 5;
const char *const OUTCOME_NAMES[OUTCOMES] = {"unaffected", "error", "wrong data", "crashed", "hung"};

constexpr int FILES = 3;
constexpr uint64_t FILE_SIZES[FILES] = {300000, 150000, 600000};
constexpr int INSERTS = 20;
constexpr uint64_t INSERT_SIZE = 64;
constexpr unsigned READ_TIMEOUT_S = 5;

compio_config small_blocks_config() {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    cfg.block_size = 1024;
    cfg.block_size__minimum = 256;
    cfg.block_size__maximum = 2048;
    cfg.b_tree_degree = 4; // many small nodes: a tree several levels deep
    return cfg;
}

std::vector<uint8_t> written_content(int file) {
    std::vector<uint8_t> data(FILE_SIZES[file]);
    for (uint64_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>((i / 97) * 31 + static_cast<uint64_t>(file) * 7 + (i >> 11));
    }
    return data;
}

uint64_t insert_position(int k) { return static_cast<uint64_t>(k) * 5000 + 123; }

// Inserts make the index carry pending key shifts, like a file that has been
// edited.
std::vector<uint8_t> expected_content(int file) {
    std::vector<uint8_t> data = written_content(file);
    for (int k = 0; k < INSERTS; k++) {
        data.insert(data.begin() + static_cast<std::ptrdiff_t>(insert_position(k)), INSERT_SIZE, 0x5A);
    }
    return data;
}

bool build_archive(const std::string &path) {
    const compio_config cfg = small_blocks_config();
    compio_archive *archive = compio_open_archive(path.c_str(), "w", &cfg);
    if (!archive) return false;
    bool ok = true;
    for (int file = 0; file < FILES && ok; file++) {
        compio_file *f = compio_open_file(("f" + std::to_string(file)).c_str(), archive);
        if (!f) return false;
        const std::vector<uint8_t> data = written_content(file);
        ok = compio_write(data.data(), data.size(), f) == data.size();
        const std::vector<uint8_t> inserted(INSERT_SIZE, 0x5A);
        for (int k = 0; k < INSERTS && ok; k++) {
            compio_seek(f, static_cast<int64_t>(insert_position(k)), COMPIO_SEEK_SET);
            ok = compio_insert(inserted.data(), INSERT_SIZE, f) == INSERT_SIZE;
        }
        compio_close_file(f);
    }
    return compio_close_archive(archive) == COMPIO_SUCCESS && ok;
}

// Runs in the child. 0: everything as written, 1: an error was reported, 2: wrong data.
int read_everything(const std::string &path) {
    const compio_config cfg = small_blocks_config();
    compio_archive *archive = compio_open_archive(path.c_str(), "r", &cfg);
    if (!archive) return 1;
    int result = 0;
    for (int file = 0; file < FILES; file++) {
        compio_file *f = compio_open_file(("f" + std::to_string(file)).c_str(), archive);
        if (!f) {
            result = std::max(result, 1);
            continue;
        }
        const std::vector<uint8_t> expected = expected_content(file);
        std::vector<uint8_t> data(expected.size());
        if (compio_get_size(f) != expected.size() ||
            compio_read(data.data(), data.size(), f) != data.size()) {
            result = std::max(result, 1);
        } else if (data != expected) {
            result = 2;
        }
        compio_close_file(f);
    }
    compio_close_archive(archive);
    return result;
}

outcome read_in_child(const std::string &path) {
    fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        // The library reports what it finds on stderr; only the outcome matters here.
        if (!freopen("/dev/null", "w", stderr)) _exit(3);
        alarm(READ_TIMEOUT_S);
        _exit(read_everything(path));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status) == SIGALRM ? outcome::hung : outcome::crashed;
    }
    switch (WEXITSTATUS(status)) {
    case 0: return outcome::unaffected;
    case 1: return outcome::error_reported;
    default: return outcome::wrong_data;
    }
}

// Addresses of all index nodes, found by walking the tree of the pristine archive.
std::vector<uint64_t> collect_nodes(const std::string &path, uint64_t &node_size) {
    std::vector<uint64_t> nodes;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return nodes;

    compio::header first, second;
    const bool first_ok = first.load_and_validate(file, 0);
    const bool second_ok = first_ok && second.load_and_validate(file, first.disk_size());
    const compio::header *active = nullptr;
    if (first_ok) active = &first;
    if (second_ok && (!first_ok || second.sequence_id > first.sequence_id)) active = &second;
    if (!active) {
        fclose(file);
        return nodes;
    }

    using namespace compio; // INDEX_NODE_SIZE names library types unqualified
    const int degree = static_cast<int>(active->b_tree_degree);
    node_size = INDEX_NODE_SIZE(degree);
    std::vector<uint64_t> pending{active->index_root};
    while (!pending.empty()) {
        const uint64_t addr = pending.back();
        pending.pop_back();
        compio::index_node node(degree);
        if (!node.read_from(file, addr)) continue;
        nodes.push_back(addr);
        if (!node.is_leaf) {
            pending.insert(pending.end(), node.children.begin(), node.children.end());
        }
    }
    fclose(file);
    return nodes;
}

bool option(const char *arg, const char *name, std::string &value) {
    const size_t len = strlen(name);
    if (strncmp(arg, name, len) != 0 || arg[len] != '=') return false;
    value = arg + len + 1;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::string dir = std::filesystem::temp_directory_path().string();
    int trials = 200;
    uint64_t seed = 1;
    for (int i = 1; i < argc; i++) {
        std::string value;
        if (option(argv[i], "--dir", value)) dir = value;
        else if (option(argv[i], "--trials", value)) trials = atoi(value.c_str());
        else if (option(argv[i], "--seed", value)) seed = strtoull(value.c_str(), nullptr, 10);
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    const std::string tag = std::to_string(getpid());
    const std::string pristine = dir + "/index_corruption_" + tag + ".compio";
    const std::string damaged = dir + "/index_corruption_" + tag + "_damaged.compio";

    if (!build_archive(pristine) || read_in_child(pristine) != outcome::unaffected) {
        fprintf(stderr, "failed to build the pristine archive in %s\n", dir.c_str());
        return 2;
    }
    uint64_t node_size = 0;
    const std::vector<uint64_t> nodes = collect_nodes(pristine, node_size);
    if (nodes.size() < 2) {
        fprintf(stderr, "failed to walk the index of the pristine archive\n");
        return 2;
    }
    printf("# %zu index nodes of %llu bytes, %d trials per kind of damage, seed %llu\n", nodes.size(),
           static_cast<unsigned long long>(node_size), trials, static_cast<unsigned long long>(seed));

    const char *const kinds[] = {"bytes", "stale", "torn"};
    std::mt19937_64 rng(seed);
    bool failed = false;

    printf("%-8s", "damage");
    for (const char *name : OUTCOME_NAMES) printf(" %11s", name);
    printf("\n");

    for (int kind = 0; kind < 3; kind++) {
        std::array<int, OUTCOMES> counts{};
        for (int trial = 0; trial < trials; trial++) {
            std::filesystem::copy_file(pristine, damaged,
                                       std::filesystem::copy_options::overwrite_existing);
            const uint64_t addr = nodes[rng() % nodes.size()];
            FILE *file = fopen(damaged.c_str(), "r+b");
            if (!file) {
                perror("fopen");
                return 2;
            }
            std::vector<uint8_t> patch;
            uint64_t patch_offset = 0;
            if (kind == 0) {
                patch.resize(1 + rng() % 16);
                for (auto &byte : patch) byte = static_cast<uint8_t>(rng());
                patch_offset = rng() % (node_size - patch.size());
            } else if (kind == 1) {
                uint64_t other = addr;
                while (other == addr) other = nodes[rng() % nodes.size()];
                patch.resize(node_size);
                fseek(file, static_cast<long>(other), SEEK_SET);
                if (fread(patch.data(), 1, patch.size(), file) != patch.size()) return 2;
            } else {
                patch_offset = 1 + rng() % (node_size - 1);
                patch.assign(node_size - patch_offset, 0);
            }
            fseek(file, static_cast<long>(addr + patch_offset), SEEK_SET);
            if (fwrite(patch.data(), 1, patch.size(), file) != patch.size()) return 2;
            fclose(file);

            counts[static_cast<int>(read_in_child(damaged))]++;
        }

        printf("%-8s", kinds[kind]);
        for (int count : counts) printf(" %11d", count);
        printf("\n");
        failed = failed || counts[static_cast<int>(outcome::wrong_data)] != 0 ||
                 counts[static_cast<int>(outcome::crashed)] != 0 ||
                 counts[static_cast<int>(outcome::hung)] != 0;
    }

    std::filesystem::remove(pristine);
    std::filesystem::remove(damaged);
    std::filesystem::remove(pristine + ".wal");
    std::filesystem::remove(damaged + ".wal");
    return failed ? 1 : 0;
}
