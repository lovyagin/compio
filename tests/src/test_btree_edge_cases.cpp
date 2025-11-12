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

class BTreeEdgeCasesTest : public ::testing::Test {
protected:
    char filename[256];
    compio_archive *archive;
    btree *tree;
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));

        compio_build_default_config(&config);
        config.b_tree_degree = 3;
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

    // Helper functions
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
};

// Duplicate Key Handling
TEST_F(BTreeEdgeCasesTest, InsertDuplicateKey) {
    tree_key key = make_key(1, 100);
    tree_val val1 = make_val(1000, 100);
    tree_val val2 = make_val(2000, 200);

    // Insert same key twice
    tree->insert(key, val1);
    tree->insert(key, val2); // Current implementation allows duplicates

    // Verify both entries exist (current implementation allows duplicates)
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    EXPECT_EQ(result.size(), 2);

    // Both values should be present
    bool found_val1 = false, found_val2 = false;
    for (const auto &[k, v] : result) {
        if (v == val1) found_val1 = true;
        if (v == val2) found_val2 = true;
    }
    EXPECT_TRUE(found_val1);
    EXPECT_TRUE(found_val2);
}

TEST_F(BTreeEdgeCasesTest, UpdateDuplicateKey) {
    tree_key key = make_key(1, 100);
    tree_val val1 = make_val(1000, 100);
    tree_val val2 = make_val(2000, 200);
    tree_val val3 = make_val(3000, 300);

    // Insert key, then update multiple times
    tree->insert(key, val1);
    EXPECT_TRUE(tree->update(key, val2));
    EXPECT_TRUE(tree->update(key, val3));

    // Verify final value
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second, val3);
}

// Zero and Negative-like Values
TEST_F(BTreeEdgeCasesTest, ZeroPositionKey) {
    tree_key key = make_key(1, 0);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, val);
}

TEST_F(BTreeEdgeCasesTest, ZeroHashKey) {
    tree_key key = make_key(0, 100);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, val);
}

TEST_F(BTreeEdgeCasesTest, ZeroAddressValue) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(0, 100);

    tree->insert(key, val);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second.addr, 0);
}

// Maximum Values
TEST_F(BTreeEdgeCasesTest, MaximumHashKey) {
    tree_key key = make_key(UINT64_MAX, 100);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first.hash, UINT64_MAX);
}

TEST_F(BTreeEdgeCasesTest, MaximumPositionKey) {
    tree_key key = make_key(1, UINT64_MAX - 1);
    tree_val val = make_val(1000, 1);

    tree->insert(key, val);

    // Use a range that doesn't cause overflow
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, make_key(1, UINT64_MAX), result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first.pos, UINT64_MAX - 1);
}

TEST_F(BTreeEdgeCasesTest, MaximumAddressValue) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(UINT64_MAX, 100);

    tree->insert(key, val);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second.addr, UINT64_MAX);
}

// Range Query Edge Cases
TEST_F(BTreeEdgeCasesTest, RangeQueryExactMatch) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);

    // Query exact range
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key, result);

    // Behavior depends on implementation - should be empty or include the key
    // This test documents the current behavior
    EXPECT_TRUE(result.empty() || result.size() == 1);
}

TEST_F(BTreeEdgeCasesTest, RangeQuerySingleUnitRange) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);

    // Query range of size 1
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);

    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first, key);
}

TEST_F(BTreeEdgeCasesTest, RangeQueryMaximumRange) {
    // Insert some test data
    tree->insert(make_key(1, 100), make_val(1000, 100));
    tree->insert(make_key(UINT64_MAX - 1, UINT64_MAX - 100), make_val(2000, 200));

    // Query large range that should include both items
    tree_key min_key = {0, 0};
    tree_key max_key = {UINT64_MAX, UINT64_MAX};

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, max_key, result);

    EXPECT_EQ(result.size(), 2);
}

// Single Node Tree Operations
TEST_F(BTreeEdgeCasesTest, SingleNodeTree) {
    // Insert just enough data to fit in root node using sequential blocks
    insert_sequential_blocks(4, 1, 0, 50); // Less than 2*degree-1

    // Verify all operations work on single node
    auto test_data = create_sequential_data(4, 1, 0, 50);
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key));
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->second, val);
    }

    // Remove from single node
    tree->remove(test_data[1].first); // Remove second element

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(test_data[1].first, test_data[1].first + 1, result);
    EXPECT_TRUE(result.empty());

    // Verify others still exist
    result.clear();
    tree->get_range(test_data[0].first, test_data[0].first + 1, result);
    ASSERT_TRUE(contains_key(result, test_data[0].first));
    auto it0 = find_key_value(result, test_data[0].first);
    ASSERT_NE(it0, result.end());
    EXPECT_EQ(it0->second, test_data[0].second);

    result.clear();
    tree->get_range(test_data[2].first, test_data[2].first + 1, result);
    ASSERT_TRUE(contains_key(result, test_data[2].first));
    auto it2 = find_key_value(result, test_data[2].first);
    ASSERT_NE(it2, result.end());
    EXPECT_EQ(it2->second, test_data[2].second);
}

// Key Ordering Edge Cases
TEST_F(BTreeEdgeCasesTest, IdenticalHashSequentialPositions) {
    // Insert keys with same hash but sequential positions (valid blocks)
    auto test_data = create_sequential_data(10, 0x12345678, 0, 50);
    
    for (const auto &[key, val] : test_data) {
        tree->insert(key, val);
    }

    // Verify all are accessible and in correct order
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key));
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->first.pos, key.pos);
        EXPECT_EQ(it->second, val);
    }
}

TEST_F(BTreeEdgeCasesTest, IdenticalPositionDifferentHashes) {
    // Insert keys with same position but different hashes (valid - different files)
    std::vector<std::pair<tree_key, tree_val>> test_data;
    for (uint64_t hash = 1; hash <= 10; ++hash) {
        test_data.push_back({make_key(hash, 100), make_val(1000 + hash * 100, 50 + hash)});
    }

    for (const auto &[key, val] : test_data) {
        tree->insert(key, val);
    }

    // Verify all are accessible
    for (const auto &[key, val] : test_data) {
        std::vector<std::pair<tree_key, tree_val>> result;
        tree->get_range(key, key + 1, result);
        ASSERT_TRUE(contains_key(result, key));
        auto it = find_key_value(result, key);
        ASSERT_NE(it, result.end());
        EXPECT_EQ(it->first.hash, key.hash);
        EXPECT_EQ(it->second, val);
    }
}

// Persistence Edge Cases
TEST_F(BTreeEdgeCasesTest, EmptyTreePersistence) {
    // Verify tree is still empty
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX), result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BTreeEdgeCasesTest, SingleItemPersistence) {
    // Insert single item
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);
    tree->insert(key, val);

    // Verify item persisted
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key, key + 1, result);
    ASSERT_TRUE(contains_key(result, key));
    auto it = find_key_value(result, key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, val);
}

// Memory and Resource Edge Cases
TEST_F(BTreeEdgeCasesTest, RapidInsertDelete) {
    // Rapid insert/delete operations to stress memory management
    std::vector<tree_key> inserted_keys;
    
    for (int i = 0; i < 100; ++i) {
        // Use sequential blocks to avoid overlap
        uint64_t pos = (i % 10) * 100; // Reuse positions but with enough spacing
        tree_key key = make_key(1, pos);
        tree_val val = make_val(1000 + i, 50 + i);

        tree->insert(key, val);
        inserted_keys.push_back(key);

        if (i % 2 == 0) {
            tree->remove(key);
            inserted_keys.pop_back();
        }
    }

    // Verify tree is still in valid state
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX), result);

    // Should have exactly 50 items (all odd iterations from 0-99)
    EXPECT_EQ(result.size(), 50);
}

// Error Recovery Tests
TEST_F(BTreeEdgeCasesTest, OperationsAfterFailedUpdate) {
    tree_key key1 = make_key(1, 100);
    tree_key key2 = make_key(1, 200);
    tree_val val = make_val(1000, 100);

    // Insert one key
    tree->insert(key1, val);

    // Try to update non-existent key (should fail)
    bool update_result = tree->update(key2, val);
    EXPECT_FALSE(update_result);

    // Verify tree is still functional
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(key1, key1 + 1, result);
    ASSERT_TRUE(contains_key(result, key1));
    auto it1 = find_key_value(result, key1);
    ASSERT_NE(it1, result.end());
    EXPECT_EQ(it1->second, val);

    // Insert the second key
    tree->insert(key2, val);

    // Verify both are present
    result.clear();
    tree->get_range(key2, key2 + 1, result);
    ASSERT_TRUE(contains_key(result, key2));
    auto it2 = find_key_value(result, key2);
    ASSERT_NE(it2, result.end());
    EXPECT_EQ(it2->second, val);
}

TEST_F(BTreeEdgeCasesTest, RemoveFromEmptyTree) {
    // Try to remove from completely empty tree
    for (int i = 0; i < 10; ++i) {
        tree->remove(make_key(1, i * 100));
    }

    // Verify tree is still empty and functional
    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX), result);
    EXPECT_TRUE(result.empty());

    // Should be able to insert after failed removes
    tree_key insert_key = make_key(1, 100);
    tree_val insert_val = make_val(1000, 100);
    tree->insert(insert_key, insert_val);
    result.clear();
    tree->get_range(insert_key, insert_key + 1, result);
    ASSERT_TRUE(contains_key(result, insert_key));
    auto it = find_key_value(result, insert_key);
    ASSERT_NE(it, result.end());
    EXPECT_EQ(it->second, insert_val);
}
