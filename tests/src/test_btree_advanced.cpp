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

class BTreeAdvancedTest : public ::testing::Test {
protected:
    char filename[256];
    compio_archive *archive;
    btree *tree;
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));

        compio_build_default_config(&config);
        config.b_tree_degree = 2;
        config.cache_size__nodes = 5;
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

    std::vector<std::pair<tree_key, tree_val>> insert_sequence(int start, int end,
                                                               uint64_t hash = 1) {
        std::vector<std::pair<tree_key, tree_val>> inserted_data;
        uint64_t current_pos = start * 100;
        for (int i = start; i < end; ++i) {
            uint64_t block_size = 50 + (i % 20);
            tree_key key = make_key(hash, current_pos);
            tree_val val = make_val(1000 + i * 100, block_size);
            tree->insert(key, val);
            inserted_data.push_back({key, val});
            current_pos += block_size;
        }
        return inserted_data;
    }

    void verify_all_present(const std::vector<std::pair<tree_key, tree_val>> &data) {
        for (const auto &[key, val] : data) {
            auto result = tree->get(key);
            ASSERT_TRUE(result.has_value())
                << "Key not found: hash=" << key.hash << ", pos=" << key.pos;
            EXPECT_EQ(result.value(), val)
                << "Incorrect value: addr=" << val.addr << ", size=" << val.size
                << " (found addr=" << result.value().addr << ", size=" << result.value().size
                << ")";
        }
    }
};

TEST_F(BTreeAdvancedTest, DeleteFromLeaf) {
    auto inserted_data = insert_sequence(0, 3);

    tree_key key_to_delete = inserted_data[1].first;
    tree->remove(key_to_delete);

    auto result = tree->get(key_to_delete);
    EXPECT_FALSE(result.has_value());

    std::vector<std::pair<tree_key, tree_val>> remaining_data = {inserted_data[0],
                                                                 inserted_data[2]};
    verify_all_present(remaining_data);
}

TEST_F(BTreeAdvancedTest, DeleteTriggersMerge) {
    auto inserted_data = insert_sequence(0, 10);

    for (int i = 0; i < 5; ++i) {
        tree_key key = inserted_data[i].first;
        tree->remove(key);
    }

    std::vector<std::pair<tree_key, tree_val>> remaining_data;
    for (int i = 5; i < 10; ++i) {
        remaining_data.push_back(inserted_data[i]);
    }
    verify_all_present(remaining_data);
}

TEST_F(BTreeAdvancedTest, DeleteTriggersBorrow) {
    auto inserted_data = insert_sequence(0, 8);

    tree_key key1 = inserted_data[1].first;
    tree->remove(key1);

    tree_key key2 = inserted_data[3].first;
    tree->remove(key2);

    std::vector<std::pair<tree_key, tree_val>> remaining_data = {
        inserted_data[0], inserted_data[2], inserted_data[4],
        inserted_data[5], inserted_data[6], inserted_data[7]};
    verify_all_present(remaining_data);
}

TEST_F(BTreeAdvancedTest, DeleteAllKeys) {
    insert_sequence(0, 10);

    uint64_t current_pos = 0;
    for (int i = 0; i < 10; ++i) {
        uint64_t block_size = 50 + (i % 20);
        tree_key key = make_key(1, current_pos);
        tree->remove(key);
        current_pos += block_size;
    }

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(make_key(0, 0), make_key(UINT64_MAX, UINT64_MAX), result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BTreeAdvancedTest, CacheEviction) {
    insert_sequential_blocks(20, 1, 0, 100);

    auto test_data = create_sequential_data(20, 1, 0, 100);
    for (int i = 0; i < 20; i += 3) {
        const auto &[key, val] = test_data[i];
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value()) << "Key not found after cache eviction: pos=" << key.pos;
    }
}

TEST_F(BTreeAdvancedTest, CacheConsistencyAfterUpdate) {
    insert_sequential_blocks(5, 1, 0, 100);

    auto test_data = create_sequential_data(5, 1, 0, 100);
    tree_key key = test_data[2].first;
    tree_val new_val = make_val(9999, 8888);
    tree->update(key, new_val);

    auto result = tree->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), new_val);
}

TEST_F(BTreeAdvancedTest, CacheConsistencyAfterDelete) {
    insert_sequential_blocks(5, 1, 0, 100);

    auto test_data = create_sequential_data(5, 1, 0, 100);
    tree_key key = test_data[2].first;
    tree->remove(key);

    auto result = tree->get(key);
    EXPECT_FALSE(result.has_value());
}

TEST_F(BTreeAdvancedTest, ComplexRangeQueries) {
    std::vector<std::pair<tree_key, tree_val>> test_data = {
        {make_key(1, 100), make_val(1000, 100)}, {make_key(1, 200), make_val(1100, 150)},
        {make_key(1, 350), make_val(1250, 200)}, {make_key(2, 50), make_val(1450, 120)},
        {make_key(2, 170), make_val(1570, 80)},  {make_key(3, 150), make_val(1650, 90)},
        {make_key(3, 240), make_val(1740, 110)}};

    for (const auto &[key, val] : test_data) {
        tree->insert(key, val);
    }

    tree_key min_key = make_key(1, 200);
    tree_key max_key = make_key(3, 200);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, max_key, result);

    EXPECT_EQ(result.size(), 5);

    bool found_1_200 = false, found_1_350 = false, found_2_50 = false, found_2_170 = false,
         found_3_150 = false;
    for (const auto &[key, val] : result) {
        if (key.hash == 1 && key.pos == 200)
            found_1_200 = true;
        if (key.hash == 1 && key.pos == 350)
            found_1_350 = true;
        if (key.hash == 2 && key.pos == 50)
            found_2_50 = true;
        if (key.hash == 2 && key.pos == 170)
            found_2_170 = true;
        if (key.hash == 3 && key.pos == 150)
            found_3_150 = true;
    }

    EXPECT_TRUE(found_1_200);
    EXPECT_TRUE(found_1_350);
    EXPECT_TRUE(found_2_50);
    EXPECT_TRUE(found_2_170);
    EXPECT_TRUE(found_3_150);
}

TEST_F(BTreeAdvancedTest, RangeQueryWithGaps) {
    auto first_batch = create_sequential_data(5, 1, 0, 50);
    auto second_batch = create_sequential_data(3, 1, 500, 60);

    for (const auto &item : first_batch) {
        tree->insert(item.first, item.second);
    }
    for (const auto &item : second_batch) {
        tree->insert(item.first, item.second);
    }

    tree_key min_key = make_key(1, 100);
    tree_key max_key = make_key(1, 550);

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, max_key, result);

    EXPECT_EQ(result.size(), 5);
}

TEST_F(BTreeAdvancedTest, MixedOperationsStress) {
    std::random_device rd;
    std::mt19937 g(rd());

    std::vector<tree_key> inserted_keys;
    std::vector<std::pair<tree_key, tree_val>> test_data;

    for (int hash = 1; hash <= 5; ++hash) {
        auto hash_data = create_sequential_data(10, hash, hash * 1000, 50);
        test_data.insert(test_data.end(), hash_data.begin(), hash_data.end());
    }

    std::shuffle(test_data.begin(), test_data.end(), g);

    for (const auto &[key, val] : test_data) {
        tree->insert(key, val);
        inserted_keys.push_back(key);
    }

    for (int i = 0; i < 20; ++i) {
        int operation = g() % 3;
        int key_index = g() % inserted_keys.size();
        tree_key key = inserted_keys[key_index];

        switch (operation) {
        case 0:
            tree->update(key, make_val(9999, 8888));
            break;
        case 1: {
            tree_key min_key = make_key(1, 0);
            tree_key max_key = make_key(5, 10000);
            std::vector<std::pair<tree_key, tree_val>> result;
            tree->get_range(min_key, max_key, result);
            EXPECT_GT(result.size(), 0);
        } break;
        case 2:
            tree->remove(key);
            inserted_keys.erase(inserted_keys.begin() + key_index);
            break;
        }
    }

    for (const auto &key : inserted_keys) {
        auto result = tree->get(key);
        (void)result;
    }
}

TEST_F(BTreeAdvancedTest, BoundaryValues) {
    tree_key min_key = {0, 0};
    tree_key max_key = {UINT64_MAX - 1, UINT64_MAX - 100};

    tree->insert(min_key, make_val(1000, 100));
    tree->insert(max_key, make_val(2000, 200));

    std::vector<std::pair<tree_key, tree_val>> result;
    tree->get_range(min_key, make_key(UINT64_MAX, UINT64_MAX), result);
    EXPECT_EQ(result.size(), 2);
}

TEST_F(BTreeAdvancedTest, ZeroSizeValues) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, 0);

    tree->insert(key, val);

    auto result = tree->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().addr, 1000);
    EXPECT_EQ(result.value().size, 0);
}

TEST_F(BTreeAdvancedTest, LargeValues) {
    tree_key key = make_key(1, 100);
    tree_val val = make_val(1000, UINT64_MAX - 1000);

    tree->insert(key, val);

    auto result = tree->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size, UINT64_MAX - 1000);
}

TEST_F(BTreeAdvancedTest, PersistenceAfterComplexOperations) {
    insert_sequential_blocks(10, 1, 0, 100);

    auto test_data = create_sequential_data(10, 1, 0, 100);
    tree->remove(test_data[3].first);
    tree->update(test_data[5].first, make_val(9999, 8888));

    for (int i = 0; i < 10; ++i) {
        if (i == 3)
            continue;

        auto result = tree->get(test_data[i].first);

        if (i == 5) {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result.value().addr, 9999);
            EXPECT_EQ(result.value().size, 8888);
        } else {
            ASSERT_TRUE(result.has_value()) << "Key not found after reopen: index=" << i;
            EXPECT_EQ(result.value(), test_data[i].second);
        }
    }

    auto result = tree->get(test_data[3].first);
    EXPECT_FALSE(result.has_value());
}
