// Real-scenario crash test: a child process writes records, flushing each, and
// is killed with SIGKILL at a random moment mid-operation. The parent reopens
// the archive and verifies it (a) always opens, (b) the surviving records form
// an uncorrupted prefix, (c) no torn record, and (d) the WAL is fully consumed.
//
// This exercises the real crash path (process killed mid-flush), guarding the
// header-vs-WAL commit ordering: the double-buffered header must never become
// durable before the data it references is committed.

#ifndef _WIN32

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

namespace {

constexpr uint64_t RECSZ = 4096;
constexpr uint64_t REC_MAGIC = 0xCAFEBABEDEADBEEFULL;

void fill_record(std::vector<uint8_t>& b, uint64_t i) {
    b.assign(RECSZ, static_cast<uint8_t>(i & 0xFF));
    std::memcpy(&b[0], &i, 8);
    std::memcpy(&b[8], &REC_MAGIC, 8);
    std::memcpy(&b[RECSZ - 8], &i, 8);
}

bool check_record(const std::vector<uint8_t>& b, uint64_t expect) {
    if (b.size() != RECSZ) return false;
    uint64_t head, magic, tail;
    std::memcpy(&head, &b[0], 8);
    std::memcpy(&magic, &b[8], 8);
    std::memcpy(&tail, &b[RECSZ - 8], 8);
    if (magic != REC_MAGIC || head != expect || tail != expect) return false;
    for (uint64_t k = 16; k < RECSZ - 8; ++k)
        if (b[k] != static_cast<uint8_t>(expect & 0xFF)) return false;
    return true;
}

bool wal_pending(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

}  // namespace

class CrashSigkillTest : public ::testing::TestWithParam<int> {
protected:
    char fn[256];
    std::string wal_fn;
    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        wal_fn = std::string(fn) + ".wal";
    }
    void TearDown() override {
        remove(fn);
        remove(wal_fn.c_str());
    }
};

TEST_P(CrashSigkillTest, SurvivorsFormUncorruptedPrefix) {
    const int sync_mode = GetParam();
    constexpr int TRIALS = 25;

    std::mt19937 rng(1234567u + static_cast<uint32_t>(sync_mode));
    std::uniform_int_distribution<int> delay_us(150, 9000);

    for (int t = 0; t < TRIALS; ++t) {
        remove(fn);
        remove(wal_fn.c_str());

        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.block_size = 4096;
        cfg.wal_sync_mode = static_cast<compio_wal_sync_mode>(sync_mode);

        {
            compio_archive* ar = compio_open_archive(fn, "w", &cfg);
            ASSERT_NE(ar, nullptr);
            compio_close_archive(ar);
        }

        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            compio_archive* ar = compio_open_archive(fn, "r+", &cfg);
            if (!ar) _exit(11);
            compio_file* f = compio_open_file("data", ar);
            if (!f) _exit(12);
            std::vector<uint8_t> b;
            for (uint64_t i = 0;; ++i) {
                fill_record(b, i);
                if (compio_write(b.data(), RECSZ, f) != RECSZ) _exit(13);
                compio_flush(ar);
            }
        }

        usleep(delay_us(rng));
        kill(pid, SIGKILL);
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);

        // The archive must ALWAYS reopen after a crash, in every sync mode --
        // a hard kill must never leave it unopenable.
        compio_archive* ar = compio_open_archive(fn, "r+", &cfg);
        ASSERT_NE(ar, nullptr) << "trial " << t << ": archive must always reopen after a crash";
        compio_file* f = compio_open_file("data", ar);
        ASSERT_NE(f, nullptr) << "trial " << t;

        // OFF deliberately skips fsync ("dangerous, for testing/temp files"), so
        // a hard crash can leave torn/lost data. Only ALWAYS and NORMAL, which
        // fsync the WAL, must preserve an uncorrupted prefix and a consumed WAL.
        if (sync_mode != COMPIO_WAL_SYNC_OFF) {
            uint64_t total = compio_get_size(f);
            ASSERT_EQ(total % RECSZ, 0u) << "trial " << t << ": no torn record (size must be record-aligned)";
            uint64_t n = total / RECSZ;
            std::vector<uint8_t> b(RECSZ);
            for (uint64_t i = 0; i < n; ++i) {
                compio_seek(f, static_cast<int64_t>(i * RECSZ), COMPIO_SEEK_SET);
                ASSERT_EQ(compio_read(b.data(), RECSZ, f), RECSZ) << "trial " << t << " rec " << i;
                ASSERT_TRUE(check_record(b, i)) << "trial " << t << ": record " << i << " corrupted";
            }
            EXPECT_FALSE(wal_pending(wal_fn)) << "trial " << t << ": WAL must be consumed by recovery";
        }
        compio_close_file(f);
        compio_close_archive(ar);
    }
}

INSTANTIATE_TEST_SUITE_P(SyncModes, CrashSigkillTest,
                         ::testing::Values(COMPIO_WAL_SYNC_ALWAYS,
                                           COMPIO_WAL_SYNC_NORMAL,
                                           COMPIO_WAL_SYNC_OFF));

#endif  // _WIN32
