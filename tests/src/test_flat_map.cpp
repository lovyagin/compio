#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "compio/detail/flat_map.hpp"

using namespace compio::detail;

TEST(FlatMapTest, BasicInsertAndFind) {
    flat_map map;
    map.insert_or_assign("key1", 100);
    map.insert_or_assign("key2", 200);

    auto p1 = map.find("key1");
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(*p1, 100);

    auto p2 = map.find("key2");
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(*p2, 200);

    auto p3 = map.find("key3");
    EXPECT_EQ(p3, nullptr);
}

TEST(FlatMapTest, UpdateExisting) {
    flat_map map;
    map.insert_or_assign("key1", 100);
    map.insert_or_assign("key1", 300); // Update

    auto p1 = map.find("key1");
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(*p1, 300);
}

TEST(FlatMapTest, Erase) {
    flat_map map;
    map.insert_or_assign("key1", 100);
    map.insert_or_assign("key2", 200);

    EXPECT_TRUE(map.erase("key1"));
    EXPECT_EQ(map.find("key1"), nullptr);
    EXPECT_NE(map.find("key2"), nullptr);
    
    // Erase non-existing
    EXPECT_FALSE(map.erase("key1"));
    EXPECT_FALSE(map.erase("key3"));
}

TEST(FlatMapTest, TombstoneReuse) {
    flat_map map;
    map.insert_or_assign("key1", 100);
    map.erase("key1");
    // "key1" is now a tombstone

    // Insert same key again
    map.insert_or_assign("key1", 500);
    auto p1 = map.find("key1");
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(*p1, 500);

    // Insert different key that might collide (hard to force without knowing hash, 
    // but general usage should work)
    map.insert_or_assign("key2", 600);
    EXPECT_NE(map.find("key2"), nullptr);
}

TEST(FlatMapTest, ReserveAndResize) {
    flat_map map;
    // Map capacity must be power of 2
    map.reserve(100);
    // 100 / 0.75 = 133.3 => next power of 2 >= 133 is 256? Or 128?
    // reserve: needed = 134. cap=16. 32, 64, 128 (not enough), 256.
    // So capacity should be >= 134.
    EXPECT_GE(map.capacity(), 128); 

    std::vector<std::string> keys;
    keys.reserve(100);

    for (int i = 0; i < 100; ++i) {
        keys.push_back("k" + std::to_string(i));
        map.insert_or_assign(keys.back(), i);
    }

    for (int i = 0; i < 100; ++i) {
        auto p = map.find(keys[i]);
        ASSERT_NE(p, nullptr) << "Key " << keys[i] << " not found";
        EXPECT_EQ(*p, i);
    }
}

TEST(FlatMapTest, Collisions) {
    // Hard to force collisions with std::hash, but inserting many keys ensures some collisions
    flat_map map;
    int n = 1000;
    // Store keys in vector to ensure they stay valid (string_view needs backing storage)
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) {
        keys.push_back(std::to_string(i));
    }
    
    for (int i = 0; i < n; ++i) {
        map.insert_or_assign(keys[i], i);
    }
    
    for (int i = 0; i < n; ++i) {
        auto p = map.find(keys[i]);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(*p, i);
    }
}

TEST(FlatMapTest, Clear) {
    flat_map map;
    map.insert_or_assign("a", 1);
    map.insert_or_assign("b", 2);
    map.clear();
    
    EXPECT_EQ(map.size(), 0);
    EXPECT_EQ(map.find("a"), nullptr);
    
    // Can reuse after clear
    map.insert_or_assign("a", 3);
    auto p = map.find("a");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 3);
}

TEST(FlatMapTest, Emplace) {
    flat_map map;
    map.emplace("key1", 100);
    auto p = map.find("key1");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
}
