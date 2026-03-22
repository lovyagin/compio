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

class BTreeTest : public ::testing::Test {
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

    tree_key make_key(uint64_t hash, uint64_t pos) { return {hash, pos}; }

    tree_val make_val(uint64_t addr, uint64_t size) { return {addr, size}; }

    void insert_sequential_blocks(int count, uint64_t hash = 1, uint64_t start_pos = 0,
                                  uint64_t base_size = 100) {
        uint64_t current_pos = start_pos;
        for (int i = 0; i < count; ++i) {
            uint64_t block_size = base_size + (i % 10);
            tree->insert(make_key(hash, current_pos), make_val(1000 + i * 100, block_size));
            current_pos += block_size;
        }
    }

    std::vector<std::pair<tree_key, tree_val>> create_sequential_data(int count, uint64_t hash = 1,
                                                                      uint64_t start_pos = 0,
                                                                      uint64_t base_size = 100) {
        std::vector<std::pair<tree_key, tree_val>> data;
        uint64_t current_pos = start_pos;
        for (int i = 0; i < count; ++i) {
            uint64_t block_size = base_size + (i % 10);
            data.push_back({make_key(hash, current_pos), make_val(1000 + i * 100, block_size)});
            current_pos += block_size;
        }
        return data;
    }

    void insert_test_data(const std::vector<std::pair<tree_key, tree_val>> &data) {
        for (const auto &[key, val] : data) {
            tree->insert(key, val);
        }
    }

    void verify_range(const tree_key &key_min, const tree_key &key_max,
                      const std::vector<std::pair<tree_key, tree_val>> &expected) {
        auto result_opt = tree->get_range(key_min, key_max);
        ASSERT_TRUE(result_opt.has_value());
        auto result = result_opt.value();

        ASSERT_EQ(result.size(), expected.size())
            << "Range query returned " << result.size() << " items, expected " << expected.size();

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

TEST_F(BTreeTest, EmptyTreeOperations) {
    auto result_opt = tree->get_range(make_key(0, 0), make_key(100, 100));
    ASSERT_TRUE(result_opt.has_value());
    EXPECT_TRUE(result_opt.value().empty());

    tree->remove(make_key(1, 1));

    tree->update(make_key(1, 1), make_val(100, 200));
}

TEST_F(BTreeTest, SingleInsert) {
    tree_key key = make_key(0x12345678, 100);
    tree_val val = make_val(1000, 500);

    tree->insert(key, val);

    auto result = tree->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);
}

TEST_F(BTreeTest, MultipleInserts) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {{make_key(1, 100), make_val(1000, 100)},
                                                            {make_key(1, 200), make_val(1100, 150)},
                                                            {make_key(1, 350), make_val(1250, 200)},
                                                            {make_key(2, 50), make_val(1450, 120)},
                                                            {make_key(2, 180), make_val(1570, 80)}};

    insert_test_data(test_data);

    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value())
            << "Key not found: hash=" << key.hash << ", pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, UpdateExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val original_val = make_val(1000, 100);
    tree_val updated_val = make_val(2000, 200);

    tree->insert(key, original_val);

    tree->update(key, updated_val);

    auto result = tree->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), updated_val);
}

TEST_F(BTreeTest, UpdateNonExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    tree->update(key, val);
}

TEST_F(BTreeTest, RemoveExistingKey) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 100);

    tree->insert(key, val);
    tree->remove(key);

    auto result = tree->get(key);
    EXPECT_FALSE(result.has_value());
}

TEST_F(BTreeTest, RemoveNonExistingKey) {
    tree_key key = make_key(1, 100);

    tree->remove(key);

    auto result_opt = tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX));
    ASSERT_TRUE(result_opt.has_value());
    EXPECT_TRUE(result_opt.value().empty());
}

TEST_F(BTreeTest, BasicRangeQuery) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)},
        {make_key(1, 200), make_val(1100, 150)},
        {make_key(1, 350), make_val(1250, 200)},
        {make_key(1, 550), make_val(1450, 120)}};

    insert_test_data(test_data);

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

    tree_key min_key = make_key(2, 0);
    tree_key max_key = make_key(3, UINT64_MAX);

    std::vector<std::pair<tree_key, tree_val>> expected = {{make_key(2, 50), make_val(1100, 150)},
                                                           {make_key(3, 200), make_val(1250, 200)}};

    verify_range(min_key, max_key, expected);
}

TEST_F(BTreeTest, EmptyRangeQuery) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)}, {make_key(1, 200), make_val(1100, 150)}};

    insert_test_data(test_data);

    tree_key min_key = make_key(1, 350);
    tree_key max_key = make_key(1, 400);

    auto result_opt = tree->get_range(min_key, max_key);
    ASSERT_TRUE(result_opt.has_value());
    EXPECT_TRUE(result_opt.value().empty());
}

TEST_F(BTreeTest, InvalidRangeQuery) {
    tree_key min_key = make_key(1, 200);
    tree_key max_key = make_key(1, 100);

    auto result_opt = tree->get_range(min_key, max_key);
    ASSERT_TRUE(result_opt.has_value());
    EXPECT_TRUE(result_opt.value().empty());
}

TEST_F(BTreeTest, SortedInsertTriggersSplits) {
    insert_sequential_blocks(20, 1, 0, 100);

    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(20, 1, 0, 100);
    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found after splits: pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, ReverseInsertTriggersSplits) {
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(20, 1, 0, 100);
    std::reverse(test_data.begin(), test_data.end());

    insert_test_data(test_data);

    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found after reverse insert: pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, RandomInsert) {
    std::vector<std::pair<tree_key, tree_val>> test_data = create_sequential_data(30, 1, 0, 100);

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(test_data.begin(), test_data.end(), g);

    insert_test_data(test_data);

    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found after random insert: pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, SameHashDifferentPositions) {
    std::vector<std::pair<tree_key, tree_val>> test_data =
        create_sequential_data(3, 0x12345678, 100, 100);

    insert_test_data(test_data);

    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found: pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, SamePositionDifferentHashes) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)},
        {make_key(2, 100), make_val(1100, 150)},
        {make_key(3, 100), make_val(1250, 200)}};

    insert_test_data(test_data);

    for (const auto &[key, val] : test_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found: hash=" << key.hash;
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeTest, LargeDataset) {
    std::vector<std::pair<tree_key, tree_val>> test_data;

    for (int hash = 1; hash <= 10; ++hash) {
        auto hash_data = create_sequential_data(10, hash, 0, 50);
        test_data.insert(test_data.end(), hash_data.begin(), hash_data.end());
    }

    insert_test_data(test_data);

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(test_data.begin(), test_data.end(), g);

    for (int i = 0; i < 20; ++i) {
        const auto &[key, val] = test_data[i];
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value())
            << "Key not found in large dataset: hash=" << key.hash << ", pos=" << key.pos;
        EXPECT_EQ(result.value(), val);
    }
}
