// Kills a writer running a mixed workload at a random moment and checks that the archive
// recovers. The workload appends stamped records to several files, runs transactions
// (some aborted), removes and recreates a file, uses batches, flushes and compacts. After
// each kill every record must be valid and in order, transactions must be atomic, and the
// recovered archive must compact and reopen.
//
// usage: mixed_kill <trials> <wal_sync_mode 0|1|2> <tiny_nodes 0|1> <max_delay_ms>
// Exit code: 0 all trials recovered, 1 a trial failed, 2 setup error.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "compio.h"

namespace {

constexpr uint64_t RECORD_SIZE = 700;
constexpr int FILE_COUNT = 6;
constexpr int TX_FIRST = 3;
constexpr int TX_SECOND = 4;
constexpr int SCRATCH = 5;
const char *const NAMES[FILE_COUNT] = {"A", "B", "C", "X", "Y", "T"};

uint8_t fill_byte(uint32_t tag, uint64_t seq) { return static_cast<uint8_t>(seq * 31 + tag); }

void fill(std::vector<uint8_t> &buf, uint32_t tag, uint64_t seq) {
    buf.assign(RECORD_SIZE, fill_byte(tag, seq));
    std::memcpy(&buf[0], &tag, sizeof(tag));
    std::memcpy(&buf[4], &seq, sizeof(seq));
    std::memcpy(&buf[RECORD_SIZE - 8], &seq, sizeof(seq));
}

bool valid(const std::vector<uint8_t> &buf, uint32_t tag, uint64_t seq) {
    uint32_t stored_tag;
    uint64_t head;
    uint64_t tail;
    std::memcpy(&stored_tag, &buf[0], sizeof(stored_tag));
    std::memcpy(&head, &buf[4], sizeof(head));
    std::memcpy(&tail, &buf[RECORD_SIZE - 8], sizeof(tail));
    if (stored_tag != tag || head != seq || tail != seq) return false;
    for (uint64_t k = 12; k < RECORD_SIZE - 8; ++k) {
        if (buf[k] != fill_byte(tag, seq)) return false;
    }
    return true;
}

[[noreturn]] void run_writer(const char *path, const compio_config *cfg, uint32_t seed, int ready_fd) {
    compio_archive *ar = compio_open_archive(path, "r+", cfg);
    if (!ar) _exit(11);
    compio_file *files[FILE_COUNT];
    uint64_t counts[FILE_COUNT];
    for (int i = 0; i < FILE_COUNT; ++i) {
        files[i] = compio_open_file(NAMES[i], ar);
        if (!files[i]) _exit(12);
        counts[i] = compio_get_size(files[i]) / RECORD_SIZE;
    }
    const char ready = 1;
    if (write(ready_fd, &ready, 1) != 1) _exit(13);

    std::mt19937 rng(seed);
    std::vector<uint8_t> buf;
    auto append = [&](int i) {
        fill(buf, static_cast<uint32_t>(i), counts[i]);
        compio_seek(files[i], 0, COMPIO_SEEK_END);
        if (compio_write(buf.data(), RECORD_SIZE, files[i]) != RECORD_SIZE) _exit(14);
        ++counts[i];
    };

    for (;;) {
        const unsigned op = rng() % 100;
        if (op < 55) {
            append(static_cast<int>(rng() % 3));
        } else if (op < 70) {
            compio_begin_transaction(ar);
            append(TX_FIRST);
            append(TX_SECOND);
            if (rng() % 4 == 0) {
                compio_abort_transaction(ar);
                --counts[TX_FIRST];
                --counts[TX_SECOND];
            } else {
                compio_commit_transaction(ar);
            }
        } else if (op < 80) {
            compio_flush(ar);
        } else if (op < 90) {
            compio_close_file(files[SCRATCH]);
            if (compio_remove_file(ar, NAMES[SCRATCH]) != 0) _exit(15);
            files[SCRATCH] = compio_open_file(NAMES[SCRATCH], ar);
            counts[SCRATCH] = 0;
            for (unsigned k = rng() % 6; k; --k) append(SCRATCH);
        } else if (op < 93) {
            compio_begin_batch(ar);
            for (int k = 0; k < 4; ++k) append(static_cast<int>(rng() % 3));
            compio_end_batch(ar);
        } else if (op < 96) {
            if (compio_defragment(ar) != 0) _exit(16);
        } else {
            append(SCRATCH);
        }
    }
}

// Returns an empty string when the recovered archive is consistent.
std::string verify(const char *path, const compio_config *cfg) {
    compio_archive *ar = compio_open_archive(path, "r+", cfg);
    if (!ar) return "reopen failed";

    std::string problem;
    uint64_t counts[FILE_COUNT] = {};
    std::vector<uint8_t> buf(RECORD_SIZE);
    for (int i = 0; i < FILE_COUNT && problem.empty(); ++i) {
        compio_file *f = compio_open_file(NAMES[i], ar);
        if (!f) {
            problem = std::string("file missing: ") + NAMES[i];
            break;
        }
        const uint64_t size = compio_get_size(f);
        counts[i] = size / RECORD_SIZE;
        if (size % RECORD_SIZE) problem = std::string("torn size: ") + NAMES[i];
        for (uint64_t k = 0; k < counts[i] && problem.empty(); ++k) {
            compio_seek(f, static_cast<int64_t>(k * RECORD_SIZE), COMPIO_SEEK_SET);
            if (compio_read(buf.data(), RECORD_SIZE, f) != RECORD_SIZE || !valid(buf, static_cast<uint32_t>(i), k)) {
                problem = std::string("bad record: ") + NAMES[i] + " #" + std::to_string(k);
            }
        }
        compio_close_file(f);
    }
    if (problem.empty() && counts[TX_FIRST] != counts[TX_SECOND]) problem = "transaction not atomic";
    if (problem.empty() && compio_defragment(ar) != 0) problem = "compaction after recovery failed";
    if (compio_close_archive(ar) != 0 && problem.empty()) problem = "close failed";

    if (problem.empty()) {
        ar = compio_open_archive(path, "r", cfg);
        if (!ar) return "reopen after close failed";
        compio_close_archive(ar);
    }
    return problem;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <trials> <wal_sync_mode 0|1|2> <tiny_nodes 0|1> <max_delay_ms>\n", argv[0]);
        return 2;
    }
    const int trials = std::atoi(argv[1]);
    const int mode = std::atoi(argv[2]);
    const bool tiny = std::atoi(argv[3]) != 0;
    const int max_delay_ms = std::max(1, std::atoi(argv[4]));

    const char *tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp ? tmp : "/tmp") + "/mixed_kill_XXXXXX";
    const int fd = mkstemp(path.data());
    if (fd < 0) return 2;
    close(fd);
    const std::string wal = path + ".wal";

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.wal_sync_mode = static_cast<compio_wal_sync_mode>(mode);
    if (tiny) {
        cfg.block_size = 512;
        cfg.block_size__minimum = 256;
        cfg.block_size__maximum = 1024;
        cfg.b_tree_degree = 2;
    }

    std::mt19937 rng(777 + mode * 10 + (tiny ? 1 : 0));
    int failed = 0;
    for (int t = 0; t < trials; ++t) {
        remove(path.c_str());
        remove(wal.c_str());
        compio_archive *ar = compio_open_archive(path.c_str(), "w", &cfg);
        if (!ar) return 2;
        for (const char *name : NAMES) compio_close_file(compio_open_file(name, ar));
        compio_close_archive(ar);

        int ready[2];
        if (pipe(ready) != 0) return 2;
        const pid_t pid = fork();
        if (pid < 0) return 2;
        if (pid == 0) {
            close(ready[0]);
            run_writer(path.c_str(), &cfg, static_cast<uint32_t>(rng()), ready[1]);
        }
        close(ready[1]);
        char signal_byte;
        const bool started = read(ready[0], &signal_byte, 1) == 1;
        close(ready[0]);
        if (started) usleep(1000 + rng() % (static_cast<unsigned>(max_delay_ms) * 1000));
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        if (!started) {
            std::printf("trial %d: writer failed to start\n", t);
            ++failed;
            continue;
        }

        const std::string problem = verify(path.c_str(), &cfg);
        if (!problem.empty()) {
            std::printf("trial %d: %s\n", t, problem.c_str());
            ++failed;
        }
    }
    remove(path.c_str());
    remove(wal.c_str());
    std::printf("wal_sync_mode=%d tiny_nodes=%d trials=%d failed=%d\n", mode, tiny ? 1 : 0, trials, failed);
    return failed == 0 ? 0 : 1;
}
