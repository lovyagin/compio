#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <vector>

#include "compio/btree.hpp"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"
#include "compio/tree_types.hpp"
#include "compio.h"

#include "test_util.hpp"

using namespace compio;

// Helper function to check if a result vector contains a specific key
inline bool contains_key(const std::vector<std::pair<tree_key, tree_val>>& result, const tree_key& key) {
    return std::any_of(result.begin(), result.end(), 
                      [&key](const auto& pair) { return pair.first == key; });
}

// Helper function to find a specific key-value pair in result
inline auto find_key_value(const std::vector<std::pair<tree_key, tree_val>>& result, const tree_key& key) {
    return std::find_if(result.begin(), result.end(), 
                       [&key](const auto& pair) { return pair.first == key; });
}

class BTreeTest : public ::testing::Test {
protected:
    char filename[256];
    compio_archive *archive;
    btree *tree;
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));

        compio_build_default_config(&config);
        config.b_tree_degree = 3; // Small degree for more splits/merges
        config.cache_size__nodes = 10;
        config.allocation_strategy = COMPIO_ALLOC_FIRST_FIT;

        archive = compio_open_archive(filename, "w+", &config);
        ASSERT_NE(archive, nullptr);
        tree = archive->index;
        ASSERT_NE(tree, nullptr);
    }

    void TearDown() override {
        if (archive) {
            compio_close_archive(archive);
        }
        remove(filename);
    }

    // Helper functions for creating test data
    tree_key make_key(uint64_t hash, uint64_t pos) { return {hash, pos}; }

    tree_val make_val(uint64_t addr, uint64_t size) { return {addr, size}; }

    // Helper function to create sequential blocks (valid B-Tree data)
    void insert_sequential_blocks(int count, uint64_t hash = 1, uint64_t start_pos = 0, uint64_t base_size = 100) {
        uint64_t current_pos = start_pos;
        for (int i = 0; i < count; ++i) {
            uint64_t block_size = base_size + (i % 10); // Vary size slightly for realism
            tree->insert(make_key(hash, current_pos), make_val(1000 + i * 100, block_size));
            current_pos += block_size;
        }
    }

    // Helper function to create test data with sequential blocks
    std::vector<std::pair<tree_key, tree_val>> create_sequential_data(int count, uint64_t hash = 1, uint64_t start_pos = 0, uint64_t base_size = 100) {
        std::vector<std::pair<tree_key, tree_val>> data;
        uint64_t current_pos = start_pos;
        for (int i = 0; i < count; ++i) {
            uint64_t block_size = base_size + (i % 10);
            data.push_back({make_key(hash, current_pos), make_val(1000 + i * 100, block_size)});
            current_pos += block_size;
        }
        return data;
    }

    // Insert test data
    void insert_test_data(const std::vector<std::pair<tree_key, tree_val>> &data) {
        for (const auto &[key, val] : data) {
            tree->insert(key, val);
        }
    }

    // Verify range query results
    void verify_range(const tree_key &key_min, const tree_key &key_max,
                      const std::vector<std::pair<tree_key, tree_val>> &expected) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key_min, key_max, result);

        ASSERT_EQ(result.size(), expected.size())
            << "Range query returned " << result.size() << " items, expected " << expected.size();

        // Sort both vectors for comparison since order isn't guaranteed
        auto sorted_result = result;
        auto sorted_expected = expected;
        std::sort(sorted_result.begin(), sorted_result.end());
        std::sort(sorted_expected.begin(), sorted_expected.end());

        for (size_t i = 0; i < sorted_result.size(); ++i) {
            EXPECT_EQ(sorted_result[i].first, sorted_expected[i].first)
                << "Key mismatch at index " << i;
            EXPECT_EQ(sorted_result[i].second, sorted_expected[i].second)
                << "Value mismatch at index " << i;
        }
    }
};

// Basic Operations Tests
TEST_F(BTreeTest, EmptyTreeOperations) {
    // Test operations on empty tree
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(100, 100), result);
    EXPECT_TRUE(result.empty());

    // Try to remove from empty tree (should not crash)
    tree->remove(make_key(1, 1));

    // Try to update non-existent key
    bool updated = tree->update(make_key(1, 1), make_val(100, 200));
    EXPECT_FALSE(updated);
}

TEST_F(BTreeTest, SingleInsert) {
    tree_key key = make_key(0x12345678, 100);
    tree_val val = make_val(1000, 500);

    tree->insert(key, val);

    // Verify through range query
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, val);
}

TEST_F(BTreeTest, MultipleInserts) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {{make_key(1, 100), make_val(1000, 100)},
                                                            {make_key(1, 200), make_val(1100, 150)},
                                                            {make_key(1, 350), make_val(1250, 200)},
                                                            {make_key(2, 50), make_val(1450, 120)},
                                                            {make_key(2, 180), make_val(1570, 80)}};

    insert_test_data(test_data);

    // Verify all data is present
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found: hash=" << key.hash << ", pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeTest, UpdateExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val original_val = make_val(1000, 100);
    tree_val updated_val = make_val(2000, 200);

    // Insert original value
    tree->insert(key, original_val);

    // Update to new value
    bool update_result = tree->update(key, updated_val);
    EXPECT_TRUE(update_result);

    // Verify update
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second, updated_val);
}

TEST_F(BTreeTest, UpdateNonExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    // Try to update without inserting first
    bool update_result = tree->update(key, val);
    EXPECT_FALSE(update_result);
}

TEST_F(BTreeTest, RemoveExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    // Insert then remove
    tree->insert(key, val);
    tree->remove(key);

    // Verify removal
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BTreeTest, RemoveNonExistingKey) {
    tree_key key = make_key(1, 100);

    // Remove without inserting (should not crash)
    tree->remove(key);

    // Verify tree is still empty
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX), result);
    EXPECT_TRUE(result.empty());
}

// Range Query Tests
TEST_F(BTreeTest, BasicRangeQuery) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)},  // covers [100, 200)
        {make_key(1, 200), make_val(1100, 150)},  // covers [200, 350)
        {make_key(1, 350), make_val(1250, 200)},  // covers [350, 550)
        {make_key(1, 550), make_val(1450, 120)}}; // covers [550, 670)

    insert_test_data(test_data);

    // Query range [150, 400) - should intersect with keys at 100, 200, and 350
    tree_key min_key = make_key(1, 150);
    tree_key max_key = make_key(1, 400);

    std::vector<std::pair<tree_key, tree_val>> expected = {{make_key(1, 100), make_val(1000, 100)},
                                                           {make_key(1, 200), make_val(1100, 150)},
                                                           {make_key(1, 350), make_val(1250, 200)}};

    verify_range(min_key, max_key, expected);
}

TEST_F(BTreeTest, RangeQueryDifferentHashes) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {{make_key(1, 100), make_val(1000, 100)},
                                                            {make_key(2, 50), make_val(1100, 150)},
                                                            {make_key(3, 200), make_val(1250, 200)},
                                                            {make_key(4, 75), make_val(1450, 120)}};

    insert_test_data(test_data);

    // Query across multiple hashes
    tree_key min_key = make_key(2, 0);
    tree_key max_key = make_key(3, UINT64_MAX);

    std::vector<std::pair<tree_key, tree_val>> expected = {{make_key(2, 50), make_val(1100, 150)},
                                                           {make_key(3, 200), make_val(1250, 200)}};

    verify_range(min_key, max_key, expected);
}

TEST_F(BTreeTest, EmptyRangeQuery) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)},  // covers [100, 200)
        {make_key(1, 200), make_val(1100, 150)}}; // covers [200, 350)

    insert_test_data(test_data);

    // Query range that doesn't intersect with any key intervals
    // Key at 100 covers [100, 200), key at 200 covers [200, 350)
    // Range [350, 400) should not intersect with either
    tree_key min_key = make_key(1, 350);
    tree_key max_key = make_key(1, 400);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, max_key, result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BTreeTest, InvalidRangeQuery) {
    // Query where max <= min (should return empty)
    tree_key min_key = make_key(1, 200);
    tree_key max_key = make_key(1, 100);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, max_key, result);
    EXPECT_TRUE(result.empty());
}

// Tree Structure Tests
TEST_F(BTreeTest, SortedInsertTriggersSplits) {
    // Insert many sequential blocks to trigger splits
    insert_sequential_blocks(20, 1, 0, 100);

    // Verify all data is still accessible by checking a few blocks
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(20, 1, 0, 100);
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found after splits: pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeTest, ReverseInsertTriggersSplits) {
    // Insert sequential blocks in reverse order (still valid, just different insertion order)
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(20, 1, 0, 100);
    std::reverse(test_data.begin(), test_data.end());
    
    insert_test_data(test_data);

    // Verify all data is still accessible
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found after reverse insert: pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeTest, RandomInsert) {
    // Insert sequential blocks in random order
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(30, 1, 0, 100);
    
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(test_data.begin(), test_data.end(), g);

    insert_test_data(test_data);

    // Verify all data is accessible
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found after random insert: pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

// Edge Cases
TEST_F(BTreeTest, SameHashDifferentPositions) {
    // Multiple keys with same hash but sequential positions (valid blocks)
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(3, 0x12345678, 100, 100);

    insert_test_data(test_data);

    // Verify all are stored correctly
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found: pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeTest, SamePositionDifferentHashes) {
    // Keys with same position but different hashes (valid - different files)
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)},
        {make_key(2, 100), make_val(1100, 150)},
        {make_key(3, 100), make_val(1250, 200)}};

    insert_test_data(test_data);

    // Verify all are stored correctly
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key)) << "Key not found: hash=" << key.hash;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeTest, LargeDataset) {
    // Test with larger dataset to stress the tree - use sequential blocks per hash
    std::vector<std::pair<tree_key, tree_val>> test_data;
    
    for (int hash = 1; hash <= 10; ++hash) {
        auto hash_data = create_sequential_data(10, hash, 0, 50);
        test_data.insert(test_data.end(), hash_data.begin(), hash_data.end());
    }

    insert_test_data(test_data);

    // Verify random subset
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(test_data.begin(), test_data.end(), g);

    for (int i = 0; i < 20; ++i) { // Check 20 random items
        const auto &[key, val] = test_data[i];
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key))
            << "Key not found in large dataset: hash=" << key.hash << ", pos=" << key.pos;
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }
}
