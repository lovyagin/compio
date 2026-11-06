#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "compio.hpp"
#include "test_util.hpp"

class CppWrapperTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    static std::string read_all(compio::File &f) {
        std::string out(f.get_size(), '\0');
        f.seek(0);
        EXPECT_EQ(f.read(out.data(), out.size()), out.size());
        return out;
    }
};

TEST_F(CppWrapperTest, InsertEraseAndSize) {
    compio::Archive ar(fn, "w");
    compio::File f = ar.open_file("data");

    const std::string base = "0123456789";
    ASSERT_EQ(f.write(base.data(), base.size()), base.size());
    EXPECT_EQ(f.get_size(), base.size());

    f.seek(4);
    const std::string ins = "ABCD";
    ASSERT_EQ(f.insert(ins.data(), ins.size()), ins.size());
    EXPECT_EQ(f.tell(), 8u);
    EXPECT_EQ(read_all(f), "0123ABCD456789");

    f.seek(2);
    ASSERT_EQ(f.erase(6), 6u);
    EXPECT_EQ(f.tell(), 2u);
    EXPECT_EQ(read_all(f), "01456789");
}

TEST_F(CppWrapperTest, DefragmentNeedsClosedFiles) {
    compio::Archive ar(fn, "w");
    {
        compio::File f = ar.open_file("data");
        const std::string data(1 << 16, 'x');
        ASSERT_EQ(f.write(data.data(), data.size()), data.size());
        EXPECT_THROW(ar.defragment(), compio::Exception);
    }
    EXPECT_NO_THROW(ar.defragment());

    compio::File f = ar.open_file("data");
    EXPECT_EQ(f.get_size(), 1u << 16);
}

TEST_F(CppWrapperTest, ClosedHandlesThrow) {
    compio::Archive ar(fn, "w");
    compio::File f = ar.open_file("data");
    f.close();
    char c = 0;
    EXPECT_THROW(f.get_size(), compio::Exception);
    EXPECT_THROW(f.insert(&c, 1), compio::Exception);
    EXPECT_THROW(f.erase(1), compio::Exception);
    ar.close();
    EXPECT_THROW(ar.defragment(), compio::Exception);
}
