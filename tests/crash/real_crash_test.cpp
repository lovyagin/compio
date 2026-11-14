// Real crash-recovery test: fork a child that writes durable records, kill it
// with SIGKILL at a random moment mid-write, reopen the archive and verify the
// survivors form an uncorrupted prefix. This exercises the *actual* crash path
// (process killed mid-operation), not a simulated on-disk state.
//
// Build:
//   g++ -std=c++17 -O2 -I. tests/crash/real_crash_test.cpp \
//       build6/libcompio.a \
//       build6/vcpkg_installed/x64-linux/lib/lib{z,lz4,zstd,brotlienc,brotlidec,brotlicommon}.a \
//       -lm -lpthread -o /tmp/real_crash_test
//
// Run:  /tmp/real_crash_test [trials] [sync_mode 0=ALWAYS 1=NORMAL 2=OFF]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include "compio.h"

static constexpr uint64_t RECSZ = 4096;
static constexpr uint64_t MAGIC = 0xCAFEBABEDEADBEEFULL;

static void fill_record(std::vector<uint8_t>& buf, uint64_t i) {
    buf.assign(RECSZ, static_cast<uint8_t>(i & 0xFF));
    std::memcpy(&buf[0], &i, 8);
    std::memcpy(&buf[8], &MAGIC, 8);
    std::memcpy(&buf[RECSZ - 8], &i, 8);
}

// returns true if buf is a valid record carrying index `expect`
static bool check_record(const std::vector<uint8_t>& buf, uint64_t expect) {
    if (buf.size() != RECSZ) return false;
    uint64_t head, magic, tail;
    std::memcpy(&head, &buf[0], 8);
    std::memcpy(&magic, &buf[8], 8);
    std::memcpy(&tail, &buf[RECSZ - 8], 8);
    if (magic != MAGIC || head != expect || tail != expect) return false;
    for (uint64_t k = 16; k < RECSZ - 8; ++k)
        if (buf[k] != static_cast<uint8_t>(expect & 0xFF)) return false;
    return true;
}

static void make_config(compio_config* cfg, int sync_mode) {
    compio_build_default_config(cfg);
    cfg->block_size = 4096;
    cfg->wal_sync_mode = static_cast<compio_wal_sync_mode>(sync_mode);
}

// child: write records forever, flushing each so it becomes durable, until killed
[[noreturn]] static void run_child(const char* path, int sync_mode) {
    compio_config cfg;
    make_config(&cfg, sync_mode);
    compio_archive* ar = compio_open_archive(path, "r+", &cfg);
    if (!ar) _exit(101);
    compio_file* f = compio_open_file("data", ar);
    if (!f) _exit(102);

    std::vector<uint8_t> buf;
    for (uint64_t i = 0;; ++i) {
        fill_record(buf, i);
        if (compio_write(buf.data(), RECSZ, f) != RECSZ) _exit(103);
        compio_flush(ar);  // force durability of record i
    }
}

struct TrialResult {
    bool reopened = false;
    bool size_aligned = false;
    uint64_t durable = 0;   // records present after reopen
    int64_t corrupt_at = -1; // first bad record index, -1 = none
    bool wal_before = false;
    bool wal_after = false;
};

// A recovered WAL is truncated to 0 bytes but the file still exists, so test
// for a non-empty WAL (pending/unrecovered records), not mere existence.
static bool wal_pending(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

static TrialResult verify(const char* path, int sync_mode) {
    TrialResult r;
    std::string wal = std::string(path) + ".wal";
    r.wal_before = wal_pending(wal);

    compio_config cfg;
    make_config(&cfg, sync_mode);
    compio_archive* ar = compio_open_archive(path, "r+", &cfg);  // triggers recovery
    if (!ar) return r;
    r.reopened = true;

    compio_file* f = compio_open_file("data", ar);
    if (f) {
        uint64_t total = compio_get_size(f);
        r.size_aligned = (total % RECSZ == 0);
        uint64_t n = total / RECSZ;
        std::vector<uint8_t> buf(RECSZ);
        for (uint64_t i = 0; i < n; ++i) {
            compio_seek(f, static_cast<int64_t>(i * RECSZ), COMPIO_SEEK_SET);
            buf.assign(RECSZ, 0);
            uint64_t got = compio_read(buf.data(), RECSZ, f);
            if (got != RECSZ || !check_record(buf, i)) { r.corrupt_at = static_cast<int64_t>(i); break; }
        }
        r.durable = n;
        compio_close_file(f);
    }
    compio_close_archive(ar);
    r.wal_after = wal_pending(wal);
    return r;
}

int main(int argc, char** argv) {
    int trials = (argc > 1) ? std::atoi(argv[1]) : 200;
    int sync_mode = (argc > 2) ? std::atoi(argv[2]) : 0;
    const char* names[] = {"ALWAYS", "NORMAL", "OFF"};
    std::string path = "/tmp/compio_real_crash.compio";

    std::mt19937 rng(static_cast<uint32_t>(::getpid()) ^ static_cast<uint32_t>(::time(nullptr)));
    std::uniform_int_distribution<int> delay_us(200, 40000);

    int failures = 0, recovered = 0;
    uint64_t max_durable = 0;
    printf("Real crash test: %d trials, sync_mode=%s\n", trials, names[sync_mode]);

    for (int t = 0; t < trials; ++t) {
        ::unlink(path.c_str());
        ::unlink((path + ".wal").c_str());
        // fresh empty archive
        {
            compio_config cfg;
            make_config(&cfg, sync_mode);
            compio_archive* ar = compio_open_archive(path.c_str(), "w", &cfg);
            if (!ar) { printf("  trial %d: cannot create archive\n", t); ++failures; continue; }
            compio_close_archive(ar);
        }

        pid_t pid = fork();
        if (pid == 0) run_child(path.c_str(), sync_mode);
        if (pid < 0) { perror("fork"); return 2; }

        ::usleep(delay_us(rng));
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);

        TrialResult r = verify(path.c_str(), sync_mode);
        if (r.wal_before) ++recovered;
        if (r.durable > max_durable) max_durable = r.durable;

        bool ok = r.reopened && r.size_aligned && r.corrupt_at < 0 && !r.wal_after;
        if (!ok) {
            ++failures;
            if (getenv("SAVE_FAIL") && failures == 1) {
                std::string ar = "cp " + path + " /tmp/fail.compio";
                std::string aw = "cp " + path + ".wal /tmp/fail.compio.wal 2>/dev/null";
                if (system(ar.c_str()) != 0 || system(aw.c_str()) != 0) {}
                printf("  [saved failing artifact to /tmp/fail.compio]\n");
            }
            printf("  FAIL trial %d: reopened=%d aligned=%d durable=%llu corrupt_at=%lld wal(before=%d after=%d)\n",
                   t, r.reopened, r.size_aligned,
                   (unsigned long long)r.durable, (long long)r.corrupt_at,
                   r.wal_before, r.wal_after);
        }
    }

    ::unlink(path.c_str());
    ::unlink((path + ".wal").c_str());

    printf("\nResult: %d/%d trials passed | crashes-with-pending-WAL: %d | max durable records: %llu\n",
           trials - failures, trials, recovered, (unsigned long long)max_durable);
    if (failures) { printf("INTEGRITY VIOLATIONS: %d\n", failures); return 1; }
    printf("All survivors formed an uncorrupted prefix. No torn writes, no corruption.\n");
    return 0;
}
