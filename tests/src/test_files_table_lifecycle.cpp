#include <gtest/gtest.h>
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
    auto* f1 = ft2.find("file1");
    EXPECT_STREQ(f1->name, "file1");
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
