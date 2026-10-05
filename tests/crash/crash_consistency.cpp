// Model-based crash consistency check.
//
// A child process applies a known sequence of operations to one logical file
// and is killed with SIGKILL at a random moment. The archive is then reopened
// in another child (so that a hang or a crash of the library is an outcome, not
// the end of the run) and the file content is compared with a reference model:
// the result is consistent if it equals the model after some prefix of the
// operations. With --flush-every=K that prefix must also be at least as long as
// the last completed flush, which is what the library promises to keep.
//
// SIGKILL keeps the page cache, so this is the mildest kind of crash: it finds
// ordering problems in the library, not lost writes after a power failure.
//
// Usage: crash_consistency [--key=value ...]
//   --dir=PATH        directory for the archive (default: system temp dir)
//   --trials=N        number of kills (default 40)
//   --ops=N           operations per trial (default 400)
//   --mix=NAME        append | overwrite | mixed (default mixed)
//   --flush-every=K   compio_flush after every K operations, 0 = never (default 1)
//   --init-kb=N       initial file size, KiB (default 1024; forced to 0 for append)
//   --sync=NAME       always | normal | off (default normal)
//   --wal=0|1         enable_wal (default 1)
//   --block=N         target block size (default 4096; min = N/4, max = 2N)
//   --degree=N        B-tree degree (default: library default)
//   --max-delay-ms=N  upper bound of the random delay before the kill (default 60)
//   --seed=N          random seed (default 1)
//
// Exit code is 0 if every trial was consistent.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "compio.h"

namespace {

enum class op_kind { overwrite, insert, erase, append };

struct operation {
    op_kind kind;
    uint64_t pos;
    uint64_t size;
    uint64_t seed;
};

enum class outcome { consistent, lost_flushed, open_failed, read_error, corrupt, crashed, hung };

struct verdict {
    int code;       // 0 read back, 1 open failed, 2 read error
    uint64_t size;
    uint64_t hash;
};

std::vector<uint8_t> payload(uint64_t size, uint64_t seed) {
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

uint64_t content_hash(const std::vector<uint8_t> &data) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const uint8_t byte : data) {
        h ^= byte;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::string option(int argc, char **argv, const std::string &key, const std::string &fallback) {
    const std::string prefix = "--" + key + "=";
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) {
            return argv[i] + prefix.size();
        }
    }
    return fallback;
}

void apply_to_model(std::vector<uint8_t> &model, const operation &op) {
    const std::vector<uint8_t> data = payload(op.size, op.seed);
    switch (op.kind) {
    case op_kind::overwrite:
        std::copy(data.begin(), data.end(), model.begin() + static_cast<std::ptrdiff_t>(op.pos));
        break;
    case op_kind::insert:
    case op_kind::append:
        model.insert(model.begin() + static_cast<std::ptrdiff_t>(op.pos), data.begin(), data.end());
        break;
    case op_kind::erase:
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(op.pos),
                    model.begin() + static_cast<std::ptrdiff_t>(op.pos + op.size));
        break;
    }
}

void apply_to_file(compio_file *file, const operation &op) {
    const std::vector<uint8_t> data = payload(op.size, op.seed);
    compio_seek(file, static_cast<int64_t>(op.pos), COMPIO_SEEK_SET);
    switch (op.kind) {
    case op_kind::overwrite:
    case op_kind::append:
        compio_write(data.data(), op.size, file);
        break;
    case op_kind::insert:
        compio_insert(data.data(), op.size, file);
        break;
    case op_kind::erase:
        compio_erase(op.size, file);
        break;
    }
}

// Runs in a child: reopen, read the file back, report through the pipe.
void verify_and_exit(const std::string &path, const compio_config &cfg, int pipe_fd) {
    alarm(10);
    verdict v{0, 0, 0};
    compio_archive *archive = compio_open_archive(path.c_str(), "r+", &cfg);
    compio_file *file = archive ? compio_open_file("data", archive) : nullptr;
    if (!file) {
        v.code = 1;
    } else {
        v.size = compio_get_size(file);
        std::vector<uint8_t> data(v.size);
        uint64_t done = 0;
        while (done < v.size) {
            const uint64_t n = compio_read(data.data() + done, std::min<uint64_t>(1 << 20, v.size - done), file);
            if (n == 0) break;
            done += n;
        }
        if (done != v.size) {
            v.code = 2;
        } else {
            v.hash = content_hash(data);
        }
    }
    if (write(pipe_fd, &v, sizeof(v)) != static_cast<ssize_t>(sizeof(v))) {
        _exit(1);
    }
    _exit(0);
}

} // namespace

int main(int argc, char **argv) {
    const std::string dir = option(argc, argv, "dir", std::filesystem::temp_directory_path().string());
    const int trials = atoi(option(argc, argv, "trials", "40").c_str());
    const int n_ops = atoi(option(argc, argv, "ops", "400").c_str());
    const std::string mix = option(argc, argv, "mix", "mixed");
    const int flush_every = atoi(option(argc, argv, "flush-every", "1").c_str());
    const std::string sync = option(argc, argv, "sync", "normal");
    const int block = atoi(option(argc, argv, "block", "4096").c_str());
    const int max_delay_ms = std::max(1, atoi(option(argc, argv, "max-delay-ms", "60").c_str()));
    const uint64_t seed = strtoull(option(argc, argv, "seed", "1").c_str(), nullptr, 10);
    const uint64_t init_size =
        mix == "append" ? 0 : strtoull(option(argc, argv, "init-kb", "1024").c_str(), nullptr, 10) << 10;
    if (mix != "append" && mix != "overwrite" && mix != "mixed") {
        fprintf(stderr, "unknown mix\n");
        return 2;
    }

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    cfg.block_size = block;
    cfg.block_size__minimum = block / 4;
    cfg.block_size__maximum = block * 2;
    cfg.b_tree_degree = atoi(option(argc, argv, "degree", std::to_string(cfg.b_tree_degree)).c_str());
    cfg.enable_wal = option(argc, argv, "wal", "1") != "0";
    cfg.wal_sync_mode = sync == "always" ? COMPIO_WAL_SYNC_ALWAYS
                        : sync == "off"  ? COMPIO_WAL_SYNC_OFF
                                         : COMPIO_WAL_SYNC_NORMAL;

    const std::string path = dir + "/crash_consistency.compio";
    std::map<outcome, int> counts;

    for (int trial = 0; trial < trials; trial++) {
        std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(trial));
        const std::vector<uint8_t> initial = payload(init_size, rng());

        // The reference model: the state after each prefix of operations.
        std::vector<uint8_t> model = initial;
        std::vector<operation> ops;
        std::map<std::pair<uint64_t, uint64_t>, int> prefix_of_state;
        prefix_of_state[{content_hash(model), model.size()}] = 0;
        for (int i = 0; i < n_ops; i++) {
            operation op{op_kind::append, 0, 1024 + rng() % 7168, rng()};
            const uint64_t roll = rng() % 100;
            if (mix == "overwrite") {
                op.kind = op_kind::overwrite;
            } else if (mix == "mixed") {
                op.kind = roll < 40 ? op_kind::overwrite
                          : roll < 65 ? op_kind::insert
                          : roll < 90 ? op_kind::erase
                                      : op_kind::append;
            }
            switch (op.kind) {
            case op_kind::overwrite:
                op.size = std::min<uint64_t>(op.size, model.size());
                op.pos = rng() % (model.size() - op.size + 1);
                break;
            case op_kind::insert:
                op.pos = rng() % (model.size() + 1);
                break;
            case op_kind::erase:
                op.size = std::min<uint64_t>(op.size, model.size() / 2);
                op.pos = rng() % (model.size() - op.size + 1);
                break;
            case op_kind::append:
                op.pos = model.size();
                break;
            }
            apply_to_model(model, op);
            ops.push_back(op);
            prefix_of_state[{content_hash(model), model.size()}] = i + 1;
        }

        remove(path.c_str());
        remove((path + ".wal").c_str());
        {
            compio_archive *archive = compio_open_archive(path.c_str(), "w", &cfg);
            if (!archive) {
                fprintf(stderr, "cannot create %s\n", path.c_str());
                return 2;
            }
            compio_file *file = compio_open_file("data", archive);
            if (init_size != 0) compio_write(initial.data(), initial.size(), file);
            compio_close_file(file);
            compio_close_archive(archive);
        }

        // The writer reports each completed flush, so the parent knows which
        // prefix the library has promised to keep.
        int progress[2];
        if (pipe(progress) != 0) return 2;
        const pid_t writer = fork();
        if (writer == 0) {
            close(progress[0]);
            compio_archive *archive = compio_open_archive(path.c_str(), "r+", &cfg);
            compio_file *file = archive ? compio_open_file("data", archive) : nullptr;
            if (!file) _exit(1);
            for (int i = 0; i < n_ops; i++) {
                apply_to_file(file, ops[static_cast<size_t>(i)]);
                if (flush_every > 0 && (i + 1) % flush_every == 0) {
                    compio_flush(archive);
                    const int done = i + 1;
                    if (write(progress[1], &done, sizeof(done)) != static_cast<ssize_t>(sizeof(done))) {
                        _exit(1);
                    }
                }
            }
            pause();
            _exit(0);
        }
        close(progress[1]);
        usleep(static_cast<useconds_t>(1000 + rng() % (static_cast<uint64_t>(max_delay_ms) * 1000)));
        kill(writer, SIGKILL);
        int status = 0;
        waitpid(writer, &status, 0);

        int flushed_prefix = 0;
        int reported = 0;
        while (read(progress[0], &reported, sizeof(reported)) == static_cast<ssize_t>(sizeof(reported))) {
            flushed_prefix = reported;
        }
        close(progress[0]);

        int result[2];
        if (pipe(result) != 0) return 2;
        const pid_t verifier = fork();
        if (verifier == 0) {
            close(result[0]);
            verify_and_exit(path, cfg, result[1]);
        }
        close(result[1]);
        verdict v{};
        const bool reported_back = read(result[0], &v, sizeof(v)) == static_cast<ssize_t>(sizeof(v));
        close(result[0]);
        waitpid(verifier, &status, 0);

        outcome o;
        if (!reported_back) {
            o = (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) ? outcome::hung : outcome::crashed;
        } else if (v.code == 1) {
            o = outcome::open_failed;
        } else if (v.code == 2) {
            o = outcome::read_error;
        } else {
            const auto it = prefix_of_state.find({v.hash, v.size});
            if (it == prefix_of_state.end()) {
                o = outcome::corrupt;
            } else {
                o = it->second >= flushed_prefix ? outcome::consistent : outcome::lost_flushed;
            }
        }
        counts[o]++;
    }

    remove(path.c_str());
    remove((path + ".wal").c_str());

    printf("mix=%s flush_every=%d sync=%s wal=%d block=%d degree=%d trials=%d\n", mix.c_str(), flush_every,
           sync.c_str(), cfg.enable_wal, block, cfg.b_tree_degree, trials);
    printf("  consistent              %d\n", counts[outcome::consistent]);
    printf("  lost flushed operations %d\n", counts[outcome::lost_flushed]);
    printf("  silent corruption       %d\n", counts[outcome::corrupt]);
    printf("  read error              %d\n", counts[outcome::read_error]);
    printf("  open failed             %d\n", counts[outcome::open_failed]);
    printf("  library crashed         %d\n", counts[outcome::crashed]);
    printf("  library hung            %d\n", counts[outcome::hung]);
    return counts[outcome::consistent] == trials ? 0 : 1;
}
