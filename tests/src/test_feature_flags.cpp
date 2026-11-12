#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"

#include "test_util.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using compio::header;

namespace {

const uint64_t unknown_bit = 1ULL << 40;

class FeatureFlagsTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }

    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    static compio_config config() {
        compio_config cfg;
        compio_build_default_config(&cfg);
        return cfg;
    }

    void create(const char *path) {
        compio_config cfg = config();
        compio_archive *ar = compio_open_archive(path, "w+", &cfg);
        ASSERT_NE(ar, nullptr);
        compio_file *f = compio_open_file("data", ar);
        ASSERT_NE(f, nullptr);
        const std::string text = "feature flags";
        ASSERT_EQ(compio_write(text.data(), text.size(), f), text.size());
        compio_close_file(f);
        ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);
    }

    // Rewrite both header slots, the way a newer version would have written them.
    template <class Edit>
    static void edit_header(const char *path, Edit edit) {
        FILE *file = fopen(path, "rb+");
        ASSERT_NE(file, nullptr);
        for (uint64_t slot : {uint64_t{0}, header::disk_size_bytes}) {
            header h;
            ASSERT_TRUE(h.load_and_validate(file, slot));
            edit(h);
            ASSERT_TRUE(h.write_checked(file, slot));
        }
        fclose(file);
    }

    static header read_header(const char *path) {
        FILE *file = fopen(path, "rb");
        header h;
        EXPECT_NE(file, nullptr);
        EXPECT_TRUE(h.load_and_validate(file, 0));
        fclose(file);
        return h;
    }

    static std::string read_data(compio_archive *ar) {
        compio_file *f = compio_open_file("data", ar);
        if (!f) return "";
        char buf[64] = {};
        const size_t n = compio_read(buf, sizeof(buf), f);
        compio_close_file(f);
        return std::string(buf, n);
    }
};

TEST_F(FeatureFlagsTest, NewArchiveCarriesNoFlagsAndOpensWithTheNewSignature) {
    create(fn);
    const header h = read_header(fn);
    EXPECT_EQ(h.magic_number, COMPIO_MAGIC_NUMBER);
    EXPECT_EQ(h.compat_flags, 0u);
    EXPECT_EQ(h.ro_compat_flags, 0u);
    EXPECT_EQ(h.incompat_flags, 0u);

    compio_config cfg = config();
    compio_archive *ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    uint64_t compat = 1, ro = 1, incompat = 1;
    ASSERT_EQ(compio_get_features(ar, &compat, &ro, &incompat), COMPIO_SUCCESS);
    EXPECT_EQ(compat, 0u);
    EXPECT_EQ(ro, 0u);
    EXPECT_EQ(incompat, 0u);
    compio_close_archive(ar);
    EXPECT_EQ(compio_get_features(nullptr, &compat, &ro, &incompat), COMPIO_ERROR);
}

TEST_F(FeatureFlagsTest, UnknownCompatFlagIsIgnoredAndKept) {
    create(fn);
    edit_header(fn, [](header &h) { h.compat_flags = unknown_bit; });

    compio_config cfg = config();
    compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(read_data(ar), "feature flags");
    compio_file *f = compio_open_file("more", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write("x", 1, f), 1u);
    compio_close_file(f);
    ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);

    EXPECT_EQ(read_header(fn).compat_flags, unknown_bit);
}

TEST_F(FeatureFlagsTest, UnknownRoCompatFlagAllowsReadingOnly) {
    create(fn);
    edit_header(fn, [](header &h) { h.ro_compat_flags = unknown_bit; });

    compio_config cfg = config();
    compio_archive *ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(read_data(ar), "feature flags");
    compio_close_archive(ar);

    for (const char *mode : {"r+", "a"}) {
        errno = 0;
        EXPECT_EQ(compio_open_archive(fn, mode, &cfg), nullptr) << mode;
        EXPECT_EQ(errno, EROFS) << mode;
    }
}

TEST_F(FeatureFlagsTest, UnknownIncompatFlagRefusesToOpen) {
    create(fn);
    edit_header(fn, [](header &h) { h.incompat_flags = unknown_bit; });

    compio_config cfg = config();
    for (const char *mode : {"r", "r+", "a"}) {
        errno = 0;
        EXPECT_EQ(compio_open_archive(fn, mode, &cfg), nullptr) << mode;
        EXPECT_EQ(errno, ENOTSUP) << mode;
    }
}

#ifndef _WIN32
TEST_F(FeatureFlagsTest, JournalOfAnUnsupportedArchiveIsNotReplayed) {
    create(fn);
    const auto slurp = [](const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string wal = std::string(fn) + ".wal";

    // Kill a writer in the middle of its transactions until it leaves a journal that
    // only recovery can apply.
    for (int attempt = 0; attempt < 50 && slurp(wal).empty(); ++attempt) {
        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            compio_config cfg = config();
            cfg.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
            compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
            if (!ar) _exit(2);
            const std::vector<char> data(64 * 1024, static_cast<char>(attempt));
            for (;;) {
                if (compio_begin_transaction(ar) != COMPIO_SUCCESS) _exit(3);
                for (int i = 0; i < 4; ++i) {
                    compio_file *f = compio_open_file(("f" + std::to_string(i)).c_str(), ar);
                    if (!f || compio_write(data.data(), data.size(), f) != data.size()) _exit(4);
                    compio_close_file(f);
                    if (i == 1) compio_flush(ar);
                }
                if (compio_commit_transaction(ar) != COMPIO_SUCCESS) _exit(5);
            }
        }
        usleep(30000 + attempt * 3000);
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        ASSERT_TRUE(WIFSIGNALED(status)) << "child exited with " << WEXITSTATUS(status);
        if (slurp(wal).empty()) {
            // The kill landed between checkpoints: recover so the next try starts clean.
            compio_config cfg = config();
            compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
            ASSERT_NE(ar, nullptr);
            compio_close_archive(ar);
        }
    }
    if (slurp(wal).empty()) GTEST_SKIP() << "could not leave a journal behind";

    edit_header(fn, [](header &h) { h.incompat_flags = unknown_bit; });
    const std::string archive_before = slurp(fn);
    const std::string wal_before = slurp(wal);

    compio_config cfg = config();
    errno = 0;
    EXPECT_EQ(compio_open_archive(fn, "r+", &cfg), nullptr);
    EXPECT_EQ(errno, ENOTSUP);
    EXPECT_TRUE(slurp(fn) == archive_before) << "opening an unsupported archive modified it";
    EXPECT_TRUE(slurp(wal) == wal_before) << "opening an unsupported archive consumed its journal";
}
#endif

TEST_F(FeatureFlagsTest, ReservedBytesSurviveARewrite) {
    create(fn);
    edit_header(fn, [](header &h) { h.reserved[3] = 0x5a; });

    compio_config cfg = config();
    compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("more", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write("x", 1, f), 1u);
    compio_close_file(f);
    ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);

    EXPECT_EQ(read_header(fn).reserved[3], 0x5a);
}

TEST_F(FeatureFlagsTest, ChecksumCoversTheReservedBytes) {
    create(fn);
    for (uint64_t slot : {uint64_t{0}, header::disk_size_bytes}) {
        FILE *file = fopen(fn, "rb+");
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(fseek(file, static_cast<long>(slot + header::slot_bytes - 1), SEEK_SET), 0);
        ASSERT_EQ(fputc(0x01, file), 0x01);
        fclose(file);
    }
    compio_config cfg = config();
    EXPECT_EQ(compio_open_archive(fn, "r", &cfg), nullptr);
}

// A power failure while a header slot is rewritten can leave the whole physical sector
// that holds it garbled (the disk rewrites a sector as a unit). The other slot must be
// out of that sector, or both headers die together.
TEST_F(FeatureFlagsTest, GarbledSectorOfTheNewestSlotLeavesTheOtherSlotUsable) {
    create(fn);
    {
        compio_config cfg = config();
        compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
        ASSERT_NE(ar, nullptr);
        compio_file *f = compio_open_file("more", ar);
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(compio_write("x", 1, f), 1u);
        compio_close_file(f);
        ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);
    }

    FILE *file = fopen(fn, "rb+");
    ASSERT_NE(file, nullptr);
    header slots[2];
    ASSERT_TRUE(slots[0].load_and_validate(file, 0));
    ASSERT_TRUE(slots[1].load_and_validate(file, header::disk_size_bytes));
    const uint64_t newest = slots[0].sequence_id > slots[1].sequence_id ? 0 : header::disk_size_bytes;

    constexpr uint64_t sector = 4096;
    const std::vector<char> garbage(sector, '\x5a');
    ASSERT_EQ(fseek(file, static_cast<long>(newest / sector * sector), SEEK_SET), 0);
    ASSERT_EQ(fwrite(garbage.data(), 1, garbage.size(), file), garbage.size());
    fclose(file);

    compio_config cfg = config();
    compio_archive *ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr) << "both header slots were lost with one sector";
    EXPECT_EQ(read_data(ar), "feature flags");
    compio_close_archive(ar);
}

TEST_F(FeatureFlagsTest, OldSignatureIsRejected) {
    create(fn);
    FILE *file = fopen(fn, "rb+");
    ASSERT_NE(file, nullptr);
    const uint8_t old_magic[4] = {0x08, 0xad, 0x9d, 0x01}; // 27110664, format v7
    for (uint64_t slot : {uint64_t{0}, header::disk_size_bytes}) {
        ASSERT_EQ(fseek(file, static_cast<long>(slot), SEEK_SET), 0);
        ASSERT_EQ(fwrite(old_magic, 1, 4, file), 4u);
    }
    fclose(file);
    compio_config cfg = config();
    EXPECT_EQ(compio_open_archive(fn, "r", &cfg), nullptr);
}

}  // namespace
