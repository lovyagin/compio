// Kills a writer at a random moment while it compacts an archive and checks
// that the data flushed before the compaction is still readable afterwards.
//
//   compaction_crash <trials> <wal> <degree> <block> <explicit> <max_delay_us>
//
// explicit=1 compacts with compio_defragment, 0 with the compaction that runs
// when the archive is flushed. Exit status is the number of damaged trials.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "compio.h"

namespace {

constexpr size_t SIZE = 1 << 20;

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto &b : v) b = static_cast<uint8_t>(rng());
    return v;
}

compio_config make_config(bool wal, int degree, int block) {
    compio_config c;
    compio_build_default_config(&c);
    c.enable_wal = wal;
    c.fragmentation_threshold = 30;
    c.b_tree_degree = degree;
    c.block_size = block;
    c.block_size__minimum = block / 4;
    c.block_size__maximum = block * 2;
    return c;
}

bool intact(const std::string &path, bool wal, const std::vector<uint8_t> &keep) {
    compio_config c = make_config(wal, 0, 0);
    c.block_size = 0;
    compio_archive *a = compio_open_archive(path.c_str(), "r+", &c);
    if (!a) return false;
    compio_file *f = compio_open_file("keep", a);
    std::vector<uint8_t> got(SIZE);
    const bool ok = f && compio_read(got.data(), SIZE, f) == SIZE && compio_get_size(f) == SIZE && got == keep;
    if (f) compio_close_file(f);
    compio_close_archive(a);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s trials wal degree block explicit max_delay_us\n", argv[0]);
        return 2;
    }
    const int trials = atoi(argv[1]);
    const bool wal = atoi(argv[2]) != 0;
    const int degree = atoi(argv[3]);
    const int block = atoi(argv[4]);
    const bool use_explicit = atoi(argv[5]) != 0;
    const int max_delay = atoi(argv[6]);

    const auto dir = std::filesystem::temp_directory_path() / ("compaction-crash-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto keep = random_bytes(SIZE, 7);
    std::mt19937 rng(12345);
    int bad = 0;

    for (int t = 0; t < trials; ++t) {
        const std::string path = (dir / ("a" + std::to_string(t) + ".compio")).string();
        compio_config c = make_config(wal, degree, block);
        {
            compio_archive *a = compio_open_archive(path.c_str(), "w+", &c);
            if (!a) return 2;
            for (int i = 0; i < 8; ++i) {
                compio_file *drop = compio_open_file(("drop" + std::to_string(i)).c_str(), a);
                const auto d = random_bytes(SIZE / 8, 100 + i);
                compio_write(d.data(), d.size(), drop);
                compio_close_file(drop);
                compio_file *k = compio_open_file("keep", a);
                compio_seek(k, 0, COMPIO_SEEK_END);
                compio_write(keep.data() + i * (SIZE / 8), SIZE / 8, k);
                compio_close_file(k);
            }
            compio_flush(a);
            for (int i = 0; i < 8; ++i) compio_remove_file(a, ("drop" + std::to_string(i)).c_str());
            compio_flush(a);

            const pid_t child = fork();
            if (child == 0) {
                if (use_explicit) compio_defragment(a);
                else compio_flush(a);
                _exit(0);
            }
            usleep(std::uniform_int_distribution<int>(0, max_delay)(rng));
            kill(child, SIGKILL);
            int status;
            waitpid(child, &status, 0);
            // The parent drops its handle without closing: it must not touch the archive.
        }
        if (!intact(path, wal, keep)) {
            ++bad;
            fprintf(stderr, "trial %d: data lost or unreadable\n", t);
        }
    }
    std::filesystem::remove_all(dir);
    printf("trials=%d wal=%d degree=%d block=%d explicit=%d damaged=%d\n", trials, wal, degree, block,
           use_explicit, bad);
    return bad;
}
