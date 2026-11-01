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
//   --scenario=NAME   operations (default) | checkpoint-remove | checkpoint-remove-flush
//   --fill-holes=0|1  release freed storage (default: library default)
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
//   --trial-start=N   first trial index, for repeating a single trial (default 0)
//   --stop-after=N    kill after N operations and their scheduled flush (0..ops)
//   --control=0|1     finish and close normally; require the final state (default 0)
//   --keep=0|1        also keep artifacts for successful trials (default 0)
//
// Each run uses a unique subdirectory of --dir. Recovery runs on a copy;
// failed trials retain the original archive, WAL and a replay command.
// --stop-after repeats an operation boundary, not a point inside an I/O call.
// A timed trial's replay command repeats the workload, not the kill timing.
// checkpoint-remove uses one unflushed deletion, a 16 KiB initial file, no
// compression or automatic compaction, and requires WAL off. A crash must
// preserve the initial file; a clean control must preserve its deletion.
// checkpoint-remove-flush stops before header publication after data fsync.
//
// Exit code is 0 if every trial was consistent.

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"

namespace {

using compio::index_node;
using compio::tree_key;
using compio::tree_val;

enum class op_kind { overwrite, insert, erase, append };

struct operation {
    op_kind kind;
    uint64_t pos;
    uint64_t size;
    uint64_t seed;
};

enum class outcome { consistent, lost_flushed, open_failed, read_error, corrupt, crashed, hung, writer_failed };

const char *outcome_name(outcome result) {
    switch (result) {
    case outcome::consistent: return "consistent";
    case outcome::lost_flushed: return "lost_flushed";
    case outcome::open_failed: return "open_failed";
    case outcome::read_error: return "read_error";
    case outcome::corrupt: return "corrupt";
    case outcome::crashed: return "crashed";
    case outcome::hung: return "hung";
    case outcome::writer_failed: return "writer_failed";
    }
    return "unknown";
}

struct progress_report {
    int completed;
    int flushed;
};

struct verdict {
    int code;       // 0 read back, 1 open failed, 2 read error, 3 file missing
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

uint64_t number(int argc, char **argv, const std::string &key, uint64_t fallback, uint64_t maximum) {
    const std::string value = option(argc, argv, key, std::to_string(fallback));
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum) {
        throw std::invalid_argument("invalid --" + key + "=" + value);
    }
    return result;
}

std::string shell_quote(const std::string &value) {
    std::string quoted = "'";
    for (char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + "'";
}

void system_check(bool ok, const char *operation) {
    if (!ok) throw std::system_error(errno, std::generic_category(), operation);
}

void report_progress(int fd, progress_report report) {
    ssize_t written;
    do {
        written = write(fd, &report, sizeof(report));
    } while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(sizeof(report))) _exit(14);
}

#ifdef COMPIO_CRASH_HEADER_CUT
struct header_cut {
    FILE *stream = nullptr;
    off_t offset = -1;
    int progress_fd = -1;
    bool synced = false;
} cut;

extern "C" int __real_fsync(int);
extern "C" int __real_fseeko(FILE *, off_t, int);

extern "C" int __wrap_fsync(int fd) {
    const int result = __real_fsync(fd);
    if (cut.stream && fd == fileno(cut.stream) && result == 0) cut.synced = true;
    return result;
}

extern "C" int __wrap_fseeko(FILE *stream, off_t offset, int whence) {
    if (stream == cut.stream && whence == SEEK_SET && offset == cut.offset) {
        if (!cut.synced) _exit(16);
        // The parent kills only after this report, before the header seek happens.
        report_progress(cut.progress_fd, {1, 0});
        for (;;) pause();
    }
    return __real_fseeko(stream, offset, whence);
}
#endif

std::vector<char> read_region(const std::string &path, uint64_t offset, size_t size) {
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    input.seekg(static_cast<std::streamoff>(offset));
    std::vector<char> bytes(size);
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    return bytes;
}

// Drain progress while waiting so a long workload cannot fill the pipe.
progress_report monitor_writer(pid_t writer, int fd, int stop_after, bool control, int delay_us,
                               int &status) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + (control || stop_after >= 0
                                            ? std::chrono::seconds(30)
                                            : std::chrono::microseconds(delay_us));
    progress_report latest{0, 0};
    bool killed = false;
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
        if (!killed && remaining.count() < 0) {
            if (kill(writer, SIGKILL) != 0 && errno != ESRCH) system_check(false, "kill writer");
            killed = true;
        }
        pollfd readable{fd, POLLIN, 0};
        const int timeout = killed ? -1 : static_cast<int>(remaining.count() + 1);
        const int ready = poll(&readable, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        system_check(ready >= 0, "poll progress");
        if (ready == 0) continue;
        progress_report report{};
        const ssize_t bytes = read(fd, &report, sizeof(report));
        if (bytes < 0 && errno == EINTR) continue;
        system_check(bytes >= 0, "read progress");
        if (bytes == 0) break;
        if (bytes != static_cast<ssize_t>(sizeof(report))) {
            throw std::runtime_error("incomplete writer progress report");
        }
        latest = report;
        if (!killed && !control && stop_after >= 0 && latest.completed == stop_after) {
            system_check(kill(writer, SIGKILL) == 0, "kill at operation boundary");
            killed = true;
        }
    }
    close(fd);
    pid_t waited;
    do {
        waited = waitpid(writer, &status, 0);
    } while (waited < 0 && errno == EINTR);
    system_check(waited == writer, "wait for writer");
    return latest;
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

bool apply_to_file(compio_file *file, const operation &op) {
    const std::vector<uint8_t> data = payload(op.size, op.seed);
    if (compio_seek(file, static_cast<int64_t>(op.pos), COMPIO_SEEK_SET) != COMPIO_SUCCESS) return false;
    switch (op.kind) {
    case op_kind::overwrite:
    case op_kind::append:
        return compio_write(data.data(), op.size, file) == op.size;
    case op_kind::insert:
        return compio_insert(data.data(), op.size, file) == op.size;
    case op_kind::erase:
        return compio_erase(op.size, file) == op.size;
    }
    return false;
}

// Runs in a child: reopen, read the file back, report through the pipe.
void verify_and_exit(const std::string &path, const compio_config &cfg, int pipe_fd, uint64_t max_size,
                     bool checkpoint_remove) {
    alarm(10);
    verdict v{0, 0, 0};
    // The removal scenario has no WAL and needs a read-only open: r+ would
    // create a missing logical file and hide whether deletion persisted.
    compio_archive *archive = compio_open_archive(path.c_str(), checkpoint_remove ? "r" : "r+", &cfg);
    compio_file *file = archive ? compio_open_file("data", archive) : nullptr;
    if (!file) {
        v.code = archive && checkpoint_remove ? 3 : 1;
    } else {
        v.size = compio_get_size(file);
        if (v.size > max_size) {
            v.code = 2;
            if (write(pipe_fd, &v, sizeof(v)) != static_cast<ssize_t>(sizeof(v))) _exit(1);
            _exit(0);
        }
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

struct verification_result {
    verdict data{};
    int status = 0;
    bool reported = false;
};

verification_result verify_archive(const std::string &path, const compio_config &cfg,
                                   uint64_t max_size, bool checkpoint_remove) {
    int descriptors[2];
    system_check(pipe(descriptors) == 0, "create result pipe");
    fflush(nullptr);
    const pid_t verifier = fork();
    system_check(verifier >= 0, "fork verifier");
    if (verifier == 0) {
        close(descriptors[0]);
        verify_and_exit(path, cfg, descriptors[1], max_size, checkpoint_remove);
    }
    close(descriptors[1]);
    verification_result result;
    ssize_t bytes;
    do {
        bytes = read(descriptors[0], &result.data, sizeof(result.data));
    } while (bytes < 0 && errno == EINTR);
    result.reported = bytes == static_cast<ssize_t>(sizeof(result.data));
    close(descriptors[0]);
    pid_t waited;
    do {
        waited = waitpid(verifier, &result.status, 0);
    } while (waited < 0 && errno == EINTR);
    system_check(waited == verifier, "wait for verifier");
    return result;
}

int run(int argc, char **argv) {
    const std::vector<std::string> keys{
        "dir", "trials", "ops", "mix", "flush-every", "init-kb", "sync", "wal", "block",
        "degree", "max-delay-ms", "seed", "trial-start", "stop-after", "control", "keep",
        "scenario", "fill-holes"};
    std::vector<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto equals = argument.find('=');
        if (argument.compare(0, 2, "--") != 0 || equals == std::string::npos ||
            equals + 1 == argument.size()) {
            throw std::invalid_argument("expected --key=value: " + argument);
        }
        const std::string key = argument.substr(2, equals - 2);
        if (std::find(keys.begin(), keys.end(), key) == keys.end() ||
            std::find(seen.begin(), seen.end(), key) != seen.end()) {
            throw std::invalid_argument("unknown or repeated option: " + argument);
        }
        seen.push_back(key);
    }
    const std::string scenario = option(argc, argv, "scenario", "operations");
    if (scenario != "operations" && scenario != "checkpoint-remove" &&
        scenario != "checkpoint-remove-flush") {
        throw std::invalid_argument("unknown scenario");
    }
    const bool header_boundary = scenario == "checkpoint-remove-flush";
#ifndef COMPIO_CRASH_HEADER_CUT
    if (header_boundary) throw std::invalid_argument("checkpoint-remove-flush requires Linux with GNU/Clang");
#endif
    const bool checkpoint_remove = scenario != "operations";
    if (checkpoint_remove) {
        for (const char *key : {"ops", "mix", "flush-every", "init-kb", "stop-after",
                                "max-delay-ms", "sync"}) {
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                throw std::invalid_argument(scenario + " excludes --" + key);
            }
        }
    }
    const std::string dir = option(argc, argv, "dir", std::filesystem::temp_directory_path().string());
    constexpr int int_max = std::numeric_limits<int>::max();
    const int trials = number(argc, argv, "trials", 40, int_max);
    const int n_ops = number(argc, argv, "ops", checkpoint_remove ? 1 : 400, int_max - 1);
    const int trial_start = number(argc, argv, "trial-start", 0, int_max - trials);
    const std::string mix = option(argc, argv, "mix", "mixed");
    const int flush_every = number(argc, argv, "flush-every", checkpoint_remove ? 0 : 1, int_max);
    const std::string sync = option(argc, argv, "sync", "normal");
    const int block = number(argc, argv, "block", 4096, int_max / 2);
    const int max_delay_ms = number(argc, argv, "max-delay-ms", 60, (int_max - 1000) / 1000);
    const uint64_t seed = number(argc, argv, "seed", 1, UINT64_MAX);
    const uint64_t init_kb = number(argc, argv, "init-kb", checkpoint_remove ? 16 : 1024, int_max);
    const uint64_t init_size = mix == "append" ? 0 : init_kb << 10;
    const bool control = number(argc, argv, "control", 0, 1) != 0;
    const bool keep = number(argc, argv, "keep", 0, 1) != 0;
    const int stop_after =
        option(argc, argv, "stop-after", "").empty()
            ? -1 : static_cast<int>(number(argc, argv, "stop-after", 0, n_ops));
    if (mix != "append" && mix != "overwrite" && mix != "mixed") {
        throw std::invalid_argument("unknown mix");
    }
    if (sync != "always" && sync != "normal" && sync != "off") {
        throw std::invalid_argument("unknown sync mode");
    }
    if (trials == 0 || block < 4 || max_delay_ms == 0 || (control && stop_after >= 0)) {
        throw std::invalid_argument(
            "require trials > 0, block >= 4, max-delay-ms > 0; control excludes stop-after");
    }
    const int boundary = checkpoint_remove && !control ? 1 : stop_after;

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    cfg.block_size = block;
    cfg.block_size__minimum = block / 4;
    cfg.block_size__maximum = block * 2;
    cfg.b_tree_degree = number(argc, argv, "degree", cfg.b_tree_degree, int_max / 96);
    if (cfg.b_tree_degree < 2) throw std::invalid_argument("degree must be at least 2");
    cfg.enable_wal = number(argc, argv, "wal", checkpoint_remove ? 0 : 1, 1) != 0;
    cfg.fill_holes_with_zeros = number(argc, argv, "fill-holes", cfg.fill_holes_with_zeros, 1) != 0;
    if (checkpoint_remove) {
        if (cfg.enable_wal) throw std::invalid_argument(scenario + " requires --wal=0");
        compio_build_dummy_compressor(&cfg.compressor);
        cfg.fragmentation_threshold = 100;
        cfg.auto_batch_size = 0;
    }
    cfg.wal_sync_mode = sync == "always" ? COMPIO_WAL_SYNC_ALWAYS
                        : sync == "off"  ? COMPIO_WAL_SYNC_OFF
                                         : COMPIO_WAL_SYNC_NORMAL;

    std::string run_template = (std::filesystem::path(dir) / "crash-consistency-XXXXXX").string();
    system_check(mkdtemp(run_template.data()) != nullptr, "create run directory");
    const std::filesystem::path run_dir = run_template;
    printf("run_dir=%s\n", run_template.c_str());
    std::map<outcome, int> counts;

    for (int trial = trial_start; trial < trial_start + trials; trial++) {
        const std::filesystem::path trial_dir = run_dir / ("trial-" + std::to_string(trial));
        std::filesystem::create_directory(trial_dir);
        const std::string path = (trial_dir / "archive.compio").string();
        std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(trial));
        const std::vector<uint8_t> initial = payload(init_size, rng());

        // The reference model: the state after each prefix of operations.
        std::vector<uint8_t> model = initial;
        std::vector<operation> ops;
        std::vector<std::pair<uint64_t, uint64_t>> states{{content_hash(model), model.size()}};
        uint64_t max_size = model.size();
        for (int i = 0; !checkpoint_remove && i < n_ops; i++) {
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
            states.emplace_back(content_hash(model), model.size());
            max_size = std::max<uint64_t>(max_size, model.size());
        }

        const int delay_us = 1000 + rng() % (static_cast<uint64_t>(max_delay_ms) * 1000);
        std::string replay = shell_quote(std::filesystem::absolute(argv[0]).string());
        std::vector<std::pair<std::string, std::string>> arguments{
            {"dir", std::filesystem::absolute(dir).string()}, {"trials", "1"},
            {"trial-start", std::to_string(trial)}, {"scenario", scenario},
            {"seed", std::to_string(seed)},
            {"wal", std::to_string(cfg.enable_wal)}, {"block", std::to_string(block)},
            {"fill-holes", std::to_string(cfg.fill_holes_with_zeros)},
            {"degree", std::to_string(cfg.b_tree_degree)},
            {"control", std::to_string(control)}, {"keep", "1"}};
        if (!checkpoint_remove) {
            arguments.insert(arguments.end(), {{"ops", std::to_string(n_ops)}, {"mix", mix},
                {"init-kb", std::to_string(init_kb)}, {"flush-every", std::to_string(flush_every)},
                {"sync", sync}, {"max-delay-ms", std::to_string(max_delay_ms)}});
        }
        for (const auto &argument : arguments) {
            replay += " " + shell_quote("--" + argument.first + "=" + argument.second);
        }
        if (stop_after >= 0) replay += " --stop-after=" + std::to_string(stop_after);
        std::ofstream manifest(trial_dir / "replay.txt");
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest << "scenario=" << scenario << "\nfill_holes=" << cfg.fill_holes_with_zeros << '\n'
                 << "mode=" << (control ? "control" : header_boundary ? "header-boundary"
                                                 : boundary >= 0 ? "boundary" : "timed") << '\n'
                 << "trial=" << trial << "\nseed=" << seed << "\ndelay_us=" << delay_us << '\n'
                 << "replay=" << replay << '\n';
        if (checkpoint_remove) {
            manifest << "checkpoint_size=" << initial.size()
                     << "\ncheckpoint_hash=" << content_hash(initial) << '\n';
        }
        manifest.flush();
        {
            compio_archive *archive = compio_open_archive(path.c_str(), "w", &cfg);
            if (!archive) {
                throw std::runtime_error("cannot create " + path);
            }
            compio_file *file = compio_open_file("data", archive);
            if (!file || (init_size != 0 &&
                          compio_write(initial.data(), initial.size(), file) != initial.size())) {
                throw std::runtime_error("cannot initialize " + path);
            }
            if (compio_close_file(file) != COMPIO_SUCCESS ||
                compio_close_archive(archive) != COMPIO_SUCCESS) {
                throw std::runtime_error("cannot close initial archive " + path);
            }
        }
        uint64_t root_offset = 0;
        std::vector<char> saved_headers;
        std::vector<char> saved_root;
        if (checkpoint_remove) {
            const std::string checkpoint_path = (trial_dir / "checkpoint.compio").string();
            std::filesystem::copy_file(path, checkpoint_path);
            const auto checkpoint = verify_archive(checkpoint_path, cfg, initial.size(), true);
            if (!checkpoint.reported || !WIFEXITED(checkpoint.status) ||
                WEXITSTATUS(checkpoint.status) != 0 || checkpoint.data.code != 0 ||
                states[0] != std::make_pair(checkpoint.data.hash, checkpoint.data.size)) {
                throw std::runtime_error("initial checkpoint verification failed: " + checkpoint_path);
            }
            if (header_boundary) {
                auto *snapshot = compio_open_archive(checkpoint_path.c_str(), "r", &cfg);
                if (!snapshot) throw std::runtime_error("cannot inspect checkpoint header");
                root_offset = readonly(snapshot->header, compio::header)->index_root;
                const auto reserved = readonly(snapshot->header, compio::header)->reserved_size();
                if (compio_close_archive(snapshot) != COMPIO_SUCCESS) {
                    throw std::runtime_error("cannot close checkpoint inspection");
                }
                saved_headers = read_region(path, 0, reserved);
                saved_root = read_region(path, root_offset, INDEX_NODE_SIZE(cfg.b_tree_degree));
            }
            manifest << "checkpoint_verified=1\n";
            manifest.flush();
        }

        int progress[2];
        system_check(pipe(progress) == 0, "create progress pipe");
        fflush(nullptr);
        const pid_t writer = fork();
        system_check(writer >= 0, "fork writer");
        if (writer == 0) {
            close(progress[0]);
            compio_archive *archive = compio_open_archive(path.c_str(), "r+", &cfg);
            if (!archive) _exit(1);
            compio_file *file = checkpoint_remove ? nullptr : compio_open_file("data", archive);
            if (!checkpoint_remove && !file) _exit(1);
            progress_report report{0, 0};
            report_progress(progress[1], report);
            if (boundary == 0) {
                for (;;) pause();
            }
            for (int i = 0; i < n_ops; i++) {
                if (checkpoint_remove) {
                    if (compio_remove_file(archive, "data") != COMPIO_SUCCESS) _exit(13);
                } else if (!apply_to_file(file, ops[static_cast<size_t>(i)])) {
                    _exit(13);
                }
#ifdef COMPIO_CRASH_HEADER_CUT
                if (header_boundary && !control) {
                    cut.stream = archive->file;
                    cut.offset = (1 - archive->current_header_slot) * archive->header->reserved_size() / 2;
                    cut.progress_fd = progress[1];
                }
#endif
                if (header_boundary && !control) {
                    compio_flush(archive);
                    _exit(17); // The header interception must be reached.
                }
                report.completed = i + 1;
                if (flush_every > 0 && (i + 1) % flush_every == 0) {
                    compio_flush(archive);
                    report.flushed = i + 1;
                }
                report_progress(progress[1], report);
                if (boundary == i + 1) {
                    for (;;) pause();
                }
            }
            if (control) {
                compio_flush(archive);
                if ((file && compio_close_file(file) != COMPIO_SUCCESS) ||
                    compio_close_archive(archive) != COMPIO_SUCCESS) {
                    _exit(15);
                }
                report.flushed = n_ops;
                report_progress(progress[1], report);
                _exit(0);
            }
            for (;;) pause();
        }
        close(progress[1]);
        int writer_status = 0;
        const progress_report completed =
            monitor_writer(writer, progress[0], boundary, control, delay_us, writer_status);
        bool headers_unchanged = true;
        if (header_boundary) {
            headers_unchanged = saved_headers == read_region(path, 0, saved_headers.size());
            manifest << "headers_unchanged=" << headers_unchanged
                     << "\nroot_unchanged=" << (saved_root == read_region(path, root_offset, saved_root.size()))
                     << "\nheader_cut_reached=" << (!control && completed.completed == 1)
                     << '\n';
        }
        const bool writer_ok =
            control ? WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0 &&
                          completed.completed == n_ops && completed.flushed == n_ops
                    : WIFSIGNALED(writer_status) && WTERMSIG(writer_status) == SIGKILL &&
                          (boundary < 0 || completed.completed == boundary) &&
                          (!header_boundary || headers_unchanged);
        manifest << "completed=" << completed.completed << "\nflushed=" << completed.flushed
                 << "\nwriter_status=" << writer_status << '\n';
        manifest.flush();

        // Reopening may replay and truncate WAL. Preserve the untouched pair.
        const std::string recovery_path = (trial_dir / "recovery.compio").string();
        std::filesystem::copy_file(path, recovery_path);
        if (std::filesystem::exists(path + ".wal")) {
            std::filesystem::copy_file(path + ".wal", recovery_path + ".wal");
        }

        const auto verification = verify_archive(recovery_path, cfg, max_size, checkpoint_remove);
        const verdict &v = verification.data;
        const int status = verification.status;

        outcome o;
        int matched_prefix = -1;
        if (!writer_ok) {
            o = outcome::writer_failed;
        } else if (!verification.reported || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            o = (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) ? outcome::hung : outcome::crashed;
        } else if (checkpoint_remove && control) {
            o = v.code == 3 ? outcome::consistent
                           : v.code == 1 ? outcome::open_failed : outcome::corrupt;
            if (o == outcome::consistent) matched_prefix = 1;
        } else if (v.code == 1) {
            o = outcome::open_failed;
        } else if (v.code == 2) {
            o = outcome::read_error;
        } else if (checkpoint_remove) {
            // An unflushed removal must leave the entire saved checkpoint.
            // Accepting the removed state would conceal checkpoint loss.
            o = v.code == 0 && states[0] == std::make_pair(v.hash, v.size)
                    ? outcome::consistent : outcome::lost_flushed;
            if (o == outcome::consistent) matched_prefix = 0;
        } else {
            const int upper = control ? n_ops
                              : stop_after >= 0 ? stop_after
                                                : std::min(n_ops, completed.completed + 1);
            for (int prefix = upper; prefix >= (control ? n_ops : 0); --prefix) {
                if (states[static_cast<size_t>(prefix)] == std::make_pair(v.hash, v.size)) {
                    matched_prefix = prefix;
                    break;
                }
            }
            if (matched_prefix < 0) {
                o = outcome::corrupt;
            } else {
                o = matched_prefix >= completed.flushed ? outcome::consistent : outcome::lost_flushed;
            }
        }
        counts[o]++;
        manifest << "verifier_status=" << status << "\nmatched_prefix=" << matched_prefix
                 << "\nverifier_code=" << v.code
                 << "\nread_size=" << v.size << "\nread_hash=" << v.hash
                 << "\noutcome=" << outcome_name(o) << '\n';
        manifest.close();
        printf("trial=%d completed=%d flushed=%d matched=%d outcome=%s\n", trial, completed.completed,
               completed.flushed, matched_prefix, outcome_name(o));
        if (keep || o != outcome::consistent) {
            printf("artifacts=%s\n", trial_dir.c_str());
        } else {
            std::filesystem::remove_all(trial_dir);
        }
    }

    if (std::filesystem::is_empty(run_dir)) std::filesystem::remove(run_dir);

    printf("scenario=%s mix=%s flush_every=%d sync=%s wal=%d fill_holes=%d block=%d degree=%d trials=%d\n",
           scenario.c_str(), mix.c_str(), flush_every, sync.c_str(), cfg.enable_wal,
           cfg.fill_holes_with_zeros, block, cfg.b_tree_degree, trials);
    printf("  consistent              %d\n", counts[outcome::consistent]);
    printf("  lost flushed operations %d\n", counts[outcome::lost_flushed]);
    printf("  silent corruption       %d\n", counts[outcome::corrupt]);
    printf("  read error              %d\n", counts[outcome::read_error]);
    printf("  open failed             %d\n", counts[outcome::open_failed]);
    printf("  library crashed         %d\n", counts[outcome::crashed]);
    printf("  library hung            %d\n", counts[outcome::hung]);
    printf("  writer failed           %d\n", counts[outcome::writer_failed]);
    return counts[outcome::consistent] == trials ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &error) {
        fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
