#include <gtest/gtest.h>
#include <utility>
#include "compio/file.hpp"

using namespace compio;

TEST(FilesTableLifecycleTest, MoveConstructorPreservesIndex) {
    files_table ft1(10);
    ft1.add("file1");
    ft1.add("file2");
    
    // Verify initial state
    ASSERT_NE(ft1.find("file1"), nullptr);
    ASSERT_NE(ft1.find("file2"), nullptr);
    
    // Move construct
    files_table ft2(std::move(ft1));
    
    // ft1 should be empty/moved-from
    EXPECT_EQ(ft1.n_files, 0);
    
    // ft2 should have files and WORKING index
    EXPECT_EQ(ft2.n_files, 2);
    EXPECT_NE(ft2.find("file1"), nullptr);
    EXPECT_NE(ft2.find("file2"), nullptr);
    
    // Verify pointers in index map point to ft2's vector data
    // The pointer returned by find() should be inside ft2.files
    auto* f1 = ft2.find("file1");
    ASSERT_NE(f1, nullptr);
    EXPECT_STREQ(f1->name, "file1");
    
    // Explicit pointer check: f1 should be within ft2.files vector storage
    // Assuming ft2.files is contiguous (std::vector)
    const files_table::file* base = ft2.files.data();
    const files_table::file* end = base + ft2.max_files;
    EXPECT_GE(f1, base);
    EXPECT_LT(f1, end);
}

TEST(FilesTableLifecycleTest, MoveAssignmentPreservesIndex) {
    files_table ft1(10);
    ft1.add("file1");
    
    files_table ft2(10);
    ft2 = std::move(ft1);
    
    EXPECT_NE(ft2.find("file1"), nullptr);
    EXPECT_EQ(ft2.n_files, 1);
}

TEST(FilesTableLifecycleTest, CopyConstructorRebuildsIndex) {
    files_table ft1(10);
    ft1.add("file1");
    
    files_table ft2(ft1);
    
    // Both should work
    EXPECT_NE(ft1.find("file1"), nullptr);
    EXPECT_NE(ft2.find("file1"), nullptr);
    
    // Verify they are independent
    ft1.remove("file1");
    EXPECT_EQ(ft1.find("file1"), nullptr);
    EXPECT_NE(ft2.find("file1"), nullptr);
}

TEST(FilesTableLifecycleTest, CopyAssignmentRebuildsIndex) {
    files_table ft1(10);
    ft1.add("file1");
    
    files_table ft2(10);
    ft2 = ft1;
    
    EXPECT_NE(ft2.find("file1"), nullptr);
    
    ft1.remove("file1");
    EXPECT_NE(ft2.find("file1"), nullptr);
}
