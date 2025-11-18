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

class BTreeRangeAddTest : public ::testing::Test {
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

    void verify_key_transformation(const std::vector<std::pair<tree_key, tree_val>> &original_data,
                                   const std::vector<tree_key> &modified_keys, int64_t offset) {
        for (const auto &[old_key, old_val] : original_data) {
            if (std::find(modified_keys.begin(), modified_keys.end(), old_key) !=
                modified_keys.end()) {
                auto result = tree->get(old_key);
                EXPECT_FALSE(result.has_value())
                    << "Old key should not exist after modification: hash=" << old_key.hash
                    << ", pos=" << old_key.pos;
            }
        }

        for (const auto &old_key : modified_keys) {
            tree_key new_key = {old_key.hash, old_key.pos + offset};
            auto result = tree->get(new_key);
            ASSERT_TRUE(result.has_value())
                << "New key not found: hash=" << new_key.hash << ", pos=" << new_key.pos;

            auto original_it =
                std::find_if(original_data.begin(), original_data.end(),
                             [&old_key](const auto &item) { return item.first == old_key; });
            ASSERT_NE(original_it, original_data.end());
            EXPECT_EQ(result.value(), original_it->second);
        }
    }

    std::vector<tree_key> get_keys_in_range(const std::vector<std::pair<tree_key, tree_val>> &data,
                                            const tree_key &lower_bound,
                                            const tree_key &upper_bound) {
        std::vector<tree_key> keys;
        for (const auto &[key, val] : data) {
            if (key >= lower_bound && key <= upper_bound) {
                keys.push_back(key);
            }
        }
        return keys;
    }
};

TEST_F(BTreeRangeAddTest, AddToSingleKeyRange) {
    auto original_data = create_sequential_data(5, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key target_key = make_key(1, 100);
    tree->add_to_range(50, target_key, target_key);

    auto old_result = tree->get(target_key);
    EXPECT_FALSE(old_result.has_value()) << "Old key should not exist after modification";

    tree_key new_key = {target_key.hash, target_key.pos + 50};
    auto new_result = tree->get(new_key);
    ASSERT_TRUE(new_result.has_value());

    auto original_it =
        std::find_if(original_data.begin(), original_data.end(),
                     [&target_key](const auto &item) { return item.first == target_key; });
    ASSERT_NE(original_it, original_data.end());
    EXPECT_EQ(new_result.value(), original_it->second);

    auto unchanged_key = tree->get(make_key(1, 0));
    ASSERT_TRUE(unchanged_key.has_value());
}

TEST_F(BTreeRangeAddTest, AddToMultipleKeysInRange) {
    auto original_data = create_sequential_data(5, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 100);
    tree_key upper_bound = make_key(1, 303);
    tree->add_to_range(50, lower_bound, upper_bound);

    auto modified_keys = get_keys_in_range(original_data, lower_bound, upper_bound);

    verify_key_transformation(original_data, modified_keys, 50);

    auto key0 = tree->get(make_key(1, 0));
    ASSERT_TRUE(key0.has_value());

    auto key406 = tree->get(make_key(1, 406));
    ASSERT_TRUE(key406.has_value());
}

TEST_F(BTreeRangeAddTest, AddNegativeValue) {
    auto original_data = create_sequential_data(3, 1, 100, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 100);
    tree_key upper_bound = make_key(1, 300);
    tree->add_to_range(-50, lower_bound, upper_bound);

    auto modified_keys = get_keys_in_range(original_data, lower_bound, upper_bound);

    verify_key_transformation(original_data, modified_keys, -50);
}

TEST_F(BTreeRangeAddTest, AddToSpecificHashOnly) {
    auto hash1_data = create_sequential_data(3, 1, 0, 100);
    auto hash2_data = create_sequential_data(3, 2, 0, 100);
    auto hash3_data = create_sequential_data(3, 3, 0, 100);

    std::vector<std::pair<tree_key, tree_val>> all_data;
    all_data.insert(all_data.end(), hash1_data.begin(), hash1_data.end());
    all_data.insert(all_data.end(), hash2_data.begin(), hash2_data.end());
    all_data.insert(all_data.end(), hash3_data.begin(), hash3_data.end());

    for (const auto &[key, val] : all_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(2, 50);
    tree_key upper_bound = make_key(2, 250);
    tree->add_to_range(50, lower_bound, upper_bound);

    auto modified_keys = get_keys_in_range(hash2_data, lower_bound, upper_bound);
    verify_key_transformation(all_data, modified_keys, 50);

    for (const auto &[key, val] : hash1_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), val);
    }

    for (const auto &[key, val] : hash3_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeRangeAddTest, AddAcrossMultipleHashes) {
    auto hash1_data = create_sequential_data(2, 1, 0, 100);
    auto hash2_data = create_sequential_data(2, 2, 0, 100);
    auto hash3_data = create_sequential_data(2, 3, 0, 100);

    std::vector<std::pair<tree_key, tree_val>> all_data;
    all_data.insert(all_data.end(), hash1_data.begin(), hash1_data.end());
    all_data.insert(all_data.end(), hash2_data.begin(), hash2_data.end());
    all_data.insert(all_data.end(), hash3_data.begin(), hash3_data.end());

    for (const auto &[key, val] : all_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 50);
    tree_key upper_bound = make_key(3, 150);
    tree->add_to_range(50, lower_bound, upper_bound);

    std::vector<tree_key> modified_keys;
    for (const auto &data : {hash1_data, hash2_data, hash3_data}) {
        auto keys_in_range = get_keys_in_range(data, lower_bound, upper_bound);
        modified_keys.insert(modified_keys.end(), keys_in_range.begin(), keys_in_range.end());
    }

    verify_key_transformation(all_data, modified_keys, 50);
}

TEST_F(BTreeRangeAddTest, AddToEmptyRange) {
    auto original_data = create_sequential_data(3, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 200);
    tree_key upper_bound = make_key(1, 100);
    tree->add_to_range(50, lower_bound, upper_bound);

    for (const auto &[key, val] : original_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeRangeAddTest, AddToNonExistentRange) {
    auto original_data = create_sequential_data(3, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 500);
    tree_key upper_bound = make_key(1, 600);
    tree->add_to_range(50, lower_bound, upper_bound);

    for (const auto &[key, val] : original_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeRangeAddTest, AddToEntireTree) {
    auto original_data = create_sequential_data(5, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 0);
    tree_key upper_bound = make_key(1, UINT64_MAX);
    tree->add_to_range(53, lower_bound, upper_bound);

    std::vector<tree_key> all_keys;
    for (const auto &[key, val] : original_data) {
        all_keys.push_back(key);
    }

    verify_key_transformation(original_data, all_keys, 53);
}

TEST_F(BTreeRangeAddTest, AddZeroValue) {
    auto original_data = create_sequential_data(3, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 0);
    tree_key upper_bound = make_key(1, 300);
    tree->add_to_range(0, lower_bound, upper_bound);

    for (const auto &[key, val] : original_data) {
        auto result = tree->get(key);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), val);
    }
}

TEST_F(BTreeRangeAddTest, MultipleRangeAddOperations) {
    auto original_data = create_sequential_data(5, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree->add_to_range(50, make_key(1, 100), make_key(1, 300));

    tree->add_to_range(-25, make_key(1, 200), make_key(1, 400));

    std::vector<std::pair<tree_key, tree_val>> current_data;
    tree->get_range(make_key(1, 0), make_key(1, UINT64_MAX), current_data);

    std::vector<uint64_t> expected_positions = {0, 150, 226, 278, 406};
    EXPECT_EQ(current_data.size(), expected_positions.size());

    for (size_t i = 0; i < current_data.size(); ++i) {
        EXPECT_EQ(current_data[i].first.pos, expected_positions[i]);
    }
}

TEST_F(BTreeRangeAddTest, RangeAddWithTreeSplits) {
    auto original_data = create_sequential_data(20, 1, 0, 101);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 200);
    tree_key upper_bound = make_key(1, 800);
    tree->add_to_range(100, lower_bound, upper_bound);

    auto modified_keys = get_keys_in_range(original_data, lower_bound, upper_bound);

    verify_key_transformation(original_data, modified_keys, 100);

    for (const auto &[key, val] : original_data) {
        if (key < lower_bound || key > upper_bound) {
            auto result = tree->get(key);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result.value(), val);
        }
    }
}

TEST_F(BTreeRangeAddTest, KeyAdditionsPropagateToChildren) {
    auto original_data = create_sequential_data(10, 1, 0, 50);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree_key lower_bound = make_key(1, 100);
    tree_key upper_bound = make_key(1, 400);
    tree->add_to_range(50, lower_bound, upper_bound);

    auto modified_keys = get_keys_in_range(original_data, lower_bound, upper_bound);

    verify_key_transformation(original_data, modified_keys, 50);
}

TEST_F(BTreeRangeAddTest, RangeAddPreservesTreeStructure) {
    auto original_data = create_sequential_data(10, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree->add_to_range(50, make_key(1, 100), make_key(1, 500));

    std::vector<std::pair<tree_key, tree_val>> range_result;
    tree->get_range(make_key(1, 0), make_key(1, 1000), range_result);
    EXPECT_EQ(range_result.size(), original_data.size());

    for (const auto &[key, val] : range_result) {
        auto individual_result = tree->get(key);
        ASSERT_TRUE(individual_result.has_value());
        EXPECT_EQ(individual_result, val);
    }

    if (!range_result.empty()) {
        tree_key test_key = range_result[0].first;
        tree_val new_val = make_val(9999, 8888);
        tree->update(test_key, new_val);

        auto verify_update = tree->get(test_key);
        ASSERT_TRUE(verify_update.has_value());
        EXPECT_EQ(verify_update, new_val);
    }
}

TEST_F(BTreeRangeAddTest, RangeAddAtBoundaries) {
    auto original_data = create_sequential_data(5, 1, 0, 100);
    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree->add_to_range(50, make_key(1, 100), make_key(1, 201));

    auto modified_keys = get_keys_in_range(original_data, make_key(1, 100), make_key(1, 201));

    verify_key_transformation(original_data, modified_keys, 50);

    auto key0 = tree->get(make_key(1, 0));
    ASSERT_TRUE(key0.has_value());

    auto key303 = tree->get(make_key(1, 303));
    ASSERT_TRUE(key303.has_value());
}

TEST_F(BTreeRangeAddTest, RangeAddWithMinimumMaximumKeys) {
    tree_key min_key = make_key(0, 0);
    tree_key max_key = make_key(UINT64_MAX, UINT64_MAX - 100);

    std::vector<std::pair<tree_key, tree_val>> original_data = {{min_key, make_val(1000, 100)},
                                                                {max_key, make_val(2000, 200)}};

    for (const auto &[key, val] : original_data) {
        tree->insert(key, val);
    }

    tree->add_to_range(50, min_key, max_key);

    auto old_min_result = tree->get(min_key);
    EXPECT_FALSE(old_min_result.has_value());

    auto old_max_result = tree->get(max_key);
    EXPECT_FALSE(old_max_result.has_value());

    tree_key new_min_key = {min_key.hash, min_key.pos + 50};
    tree_key new_max_key = {max_key.hash, max_key.pos + 50};

    auto new_min_result = tree->get(new_min_key);
    ASSERT_TRUE(new_min_result.has_value());
    EXPECT_EQ(new_min_result.value(), original_data[0].second);

    auto new_max_result = tree->get(new_max_key);
    ASSERT_TRUE(new_max_result.has_value());
    EXPECT_EQ(new_max_result.value(), original_data[1].second);
}

struct RandomizedRangeAddTestParams {
    int num_keys;
    int n_iterations;
    double check_probability;
    int num_checked_keys;
};

class BTreeRangeAddRandomizedTest : public ::testing::TestWithParam<RandomizedRangeAddTestParams> {
protected:
    char filename[256];
    compio_archive *archive;
    btree *tree;
    compio_config config;
    std::mt19937 rng;

    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));

        compio_build_default_config(&config);
        config.b_tree_degree = 3;
        config.cache_size__nodes = 5000;
        config.allocation_strategy = COMPIO_ALLOC_FIRST_FIT;

        archive = compio_open_archive(filename, "w+", &config);
        ASSERT_NE(archive, nullptr);
        tree = archive->index;
        ASSERT_NE(tree, nullptr);

        rng = std::mt19937(42);
    }

    void TearDown() override {
        if (archive) {
            compio_close_archive(archive);
        }
        remove(filename);
    }

    tree_key make_key(uint64_t hash, uint64_t pos) { return {hash, pos}; }

    tree_val make_val(uint64_t addr, uint64_t size) { return {addr, size}; }

    std::vector<std::pair<tree_key, tree_val>>
    create_sequential_keys_with_gaps(int count, uint64_t hash = 42, uint64_t start_pos = 1000,
                                     uint64_t gap_size = 500) {
        std::vector<std::pair<tree_key, tree_val>> data;
        uint64_t current_pos = start_pos;
        std::uniform_int_distribution<uint64_t> val_dist(0, UINT64_MAX);

        for (int i = 0; i < count; ++i) {
            data.push_back({make_key(hash, current_pos), make_val(val_dist(rng), val_dist(rng))});
            current_pos += gap_size;
        }
        return data;
    }

    int64_t generate_addition(const std::vector<std::pair<tree_key, tree_val>> &data,
                              tree_key lower_bound, tree_key upper_bound) {
        std::size_t idx = 0;
        while (idx < data.size() && data[idx].first < lower_bound) {
            ++idx;
        }

        int64_t min_addition;
        if (idx == 0) {
            min_addition = -data[idx].first.pos;
        } else {
            min_addition = data[idx - 1].first.pos - data[idx].first.pos + 1;
        }

        while (idx < data.size() && data[idx].first <= upper_bound) {
            ++idx;
        }

        int64_t max_addition;
        if (idx == data.size() || idx == 0) {
            max_addition = 100;
        } else {
            max_addition = data[idx].first.pos - data[idx - 1].first.pos - 1;
        }

        if (min_addition > max_addition) {
            throw std::runtime_error("failed to generate addition");
        }

        std::uniform_int_distribution<int64_t> addition_dist(min_addition, max_addition);
        return addition_dist(rng);
    }

    void validate_random_keys(const std::vector<std::pair<tree_key, tree_val>> &data,
                              int num_to_check) {
        if (data.empty() || num_to_check <= 0)
            return;

        std::uniform_int_distribution<int> dist(0, data.size() - 1);

        for (int i = 0; i < std::min(num_to_check, static_cast<int>(data.size())); ++i) {
            int idx = dist(rng);
            const auto &[expected_key, expected_val] = data[idx];

            auto result = tree->get(expected_key);
            ASSERT_TRUE(result.has_value()) << "Expected key not found: hash=" << expected_key.hash
                                            << ", pos=" << expected_key.pos;
            EXPECT_EQ(result.value(), expected_val)
                << "Value mismatch for key: hash=" << expected_key.hash
                << ", pos=" << expected_key.pos;
        }
    }

    void update_points(std::vector<std::pair<tree_key, tree_val>> &data, tree_key lower_bound,
                       tree_key upper_bound, int64_t value) {
        std::size_t idx = 0;
        while (idx < data.size() && data[idx].first < lower_bound) {
            ++idx;
        }

        while (idx < data.size() && data[idx].first <= upper_bound) {
            data[idx].first = data[idx].first + value;
            ++idx;
        }

        for (std::size_t i = 1; i < data.size(); ++i) {
            if (data[i].first <= data[i - 1].first) {
                throw std::runtime_error("keys not sorted after update_points");
            }
        }
    }
};

TEST_P(BTreeRangeAddRandomizedTest, RandomizedRangeAddOperations) {
    auto params = GetParam();

    auto original_data = create_sequential_keys_with_gaps(params.num_keys);

    std::vector<std::pair<tree_key, tree_val>> shuffled_data = original_data;
    std::shuffle(shuffled_data.begin(), shuffled_data.end(), rng);

    for (const auto &[key, val] : shuffled_data) {
        tree->insert(key, val);
    }

    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);

    for (int iteration = 0; iteration < params.n_iterations; ++iteration) {
        std::uniform_int_distribution<int> range_dist(0, original_data.size() - 1);
        tree_key lower_bound = original_data[range_dist(rng)].first;
        tree_key upper_bound = original_data[range_dist(rng)].first;

        if (lower_bound > upper_bound) {
            std::swap(lower_bound, upper_bound);
        }

        int64_t addition = generate_addition(original_data, lower_bound, upper_bound);

        tree->add_to_range(addition, lower_bound, upper_bound);

        update_points(original_data, lower_bound, upper_bound, addition);

        if (prob_dist(rng) < params.check_probability) {
            validate_random_keys(original_data, params.num_checked_keys);
        }
    }

    validate_random_keys(original_data, params.num_checked_keys);
}

INSTANTIATE_TEST_SUITE_P(BTreeRandomizedAddRange, BTreeRangeAddRandomizedTest,
                         ::testing::Values(RandomizedRangeAddTestParams{10, 100, 1.0, 10},
                                           RandomizedRangeAddTestParams{100, 1000, 1.0, 100},
                                           RandomizedRangeAddTestParams{100, 1000, 0.1, 10},
                                           RandomizedRangeAddTestParams{1000, 250, 1.0, 1000},
                                           RandomizedRangeAddTestParams{1000, 250, 0.0, 1000},
                                           RandomizedRangeAddTestParams{1000, 250, 0.1, 10},
                                           RandomizedRangeAddTestParams{2000, 100, 1.0, 2000},
                                           RandomizedRangeAddTestParams{2000, 100, 0.0, 2000}));
