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

class BTreeGetBlockTest : public ::testing::Test {
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

    void insert_block(uint64_t hash, uint64_t pos, uint64_t size, uint64_t addr = 1000) {
        tree->insert(make_key(hash, pos), make_val(addr, size));
    }

    std::vector<std::pair<tree_key, tree_val>> create_sequential_blocks(int count,
                                                                        uint64_t hash = 1,
                                                                        uint64_t start_pos = 0,
                                                                        uint64_t base_size = 100) {
        std::vector<std::pair<tree_key, tree_val>> blocks;
        uint64_t current_pos = start_pos;
        for (int i = 0; i < count; ++i) {
            uint64_t block_size = base_size + (i % 10);
            blocks.push_back({make_key(hash, current_pos), make_val(1000 + i * 100, block_size)});
            current_pos += block_size;
        }
        return blocks;
    }

    std::vector<std::pair<tree_key, tree_val>> insert_sequential_blocks(int count,
                                                                        uint64_t hash = 1,
                                                                        uint64_t start_pos = 0,
                                                                        uint64_t base_size = 100) {
        auto blocks = create_sequential_blocks(count, hash, start_pos, base_size);
        for (const auto &[key, val] : blocks) {
            tree->insert(key, val);
        }
        return blocks;
    }

    void verify_get_block(uint64_t search_hash, uint64_t search_pos,
                          const std::optional<std::pair<tree_key, tree_val>> &expected) {
        auto result = tree->get_block(make_key(search_hash, search_pos));

        if (expected.has_value()) {
            ASSERT_TRUE(result.has_value())
                << "Expected to find block for hash=" << search_hash << ", pos=" << search_pos;
            EXPECT_EQ(result->first, expected->first)
                << "Key mismatch for hash=" << search_hash << ", pos=" << search_pos;
            EXPECT_EQ(result->second, expected->second)
                << "Value mismatch for hash=" << search_hash << ", pos=" << search_pos;
        } else {
            EXPECT_FALSE(result.has_value())
                << "Expected no block to be found for hash=" << search_hash
                << ", pos=" << search_pos;
        }
    }
};

TEST_F(BTreeGetBlockTest, EmptyTree) {
    verify_get_block(1, 100, std::nullopt);
    verify_get_block(0, 0, std::nullopt);
    verify_get_block(0, UINT64_MAX - 1, std::nullopt);
}

TEST_F(BTreeGetBlockTest, SingleBlockBasicCases) {
    // Insert a single block: hash=1, pos=100, size=200
    tree_key block_key = make_key(1, 100);
    tree_val block_val = make_val(1000, 200);
    tree->insert(block_key, block_val);

    // Test positions inside the block
    verify_get_block(1, 100, {{block_key, block_val}}); // At start
    verify_get_block(1, 150, {{block_key, block_val}}); // In middle
    verify_get_block(1, 299, {{block_key, block_val}}); // At end (100 + 200 - 1)

    // Test positions outside the block
    verify_get_block(1, 99, std::nullopt);  // Before start
    verify_get_block(1, 300, std::nullopt); // After end

    // Test different hash
    verify_get_block(2, 150, std::nullopt);
}

TEST_F(BTreeGetBlockTest, SingleBlockZeroSize) {
    // Insert a zero-size block
    tree_key block_key = make_key(1, 100);
    tree_val block_val = make_val(1000, 0);
    tree->insert(block_key, block_val);

    // Zero-size block should be empty
    verify_get_block(1, 100, std::nullopt);
    verify_get_block(1, 99, std::nullopt);
    verify_get_block(1, 101, std::nullopt);
}

TEST_F(BTreeGetBlockTest, SingleBlockSizeOne) {
    // Insert a zero-size block
    tree_key block_key = make_key(1, 100);
    tree_val block_val = make_val(1000, 1);
    tree->insert(block_key, block_val);

    // Zero-size block should contain only it's start position
    verify_get_block(1, 100, std::make_pair(block_key, block_val));
    verify_get_block(1, 99, std::nullopt);
    verify_get_block(1, 101, std::nullopt);
}

TEST_F(BTreeGetBlockTest, MultipleBlocksSameHash) {
    // Insert multiple blocks with same hash
    insert_block(1, 100, 50);  // Block 1: [100, 150)
    insert_block(1, 200, 100); // Block 2: [200, 300)
    insert_block(1, 350, 75);  // Block 3: [350, 425)

    // Test positions in each block
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(1, 250, {{make_key(1, 200), make_val(1000, 100)}});
    verify_get_block(1, 400, {{make_key(1, 350), make_val(1000, 75)}});

    // Test positions in gaps
    verify_get_block(1, 175, std::nullopt); // Between block 1 and 2
    verify_get_block(1, 325, std::nullopt); // Between block 2 and 3

    // Test boundaries
    verify_get_block(1, 149, {{make_key(1, 100), make_val(1000, 50)}});  // End of block 1
    verify_get_block(1, 150, std::nullopt);                              // Start of gap
    verify_get_block(1, 199, std::nullopt);                              // End of gap
    verify_get_block(1, 200, {{make_key(1, 200), make_val(1000, 100)}}); // Start of block 2
}

TEST_F(BTreeGetBlockTest, MultipleBlocksDifferentHashes) {
    // Insert blocks with different hashes
    insert_block(1, 100, 50);
    insert_block(2, 150, 75);
    insert_block(3, 250, 100);

    // Each hash should only find its own block
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(2, 175, {{make_key(2, 150), make_val(1000, 75)}});
    verify_get_block(3, 300, {{make_key(3, 250), make_val(1000, 100)}});

    // Cross-hash searches should fail
    verify_get_block(1, 175, std::nullopt); // Hash 1, pos in hash 2 block
    verify_get_block(2, 125, std::nullopt); // Hash 2, pos in hash 1 block
}

TEST_F(BTreeGetBlockTest, OverlappingBlocksSameHash) {
    // Insert overlapping blocks with same hash
    insert_block(1, 100, 100); // [100, 200)
    insert_block(1, 150, 100); // [150, 250) - overlaps with first
    insert_block(1, 200, 50);  // [200, 250) - overlaps with second

    // Test positions in overlapping regions
    // The behavior should be deterministic - should find one of the containing blocks
    auto result_175 = tree->get_block(make_key(1, 175));
    ASSERT_TRUE(result_175.has_value());
    EXPECT_EQ(result_175->first.hash, 1);
    EXPECT_LE(result_175->first.pos, 175);
    EXPECT_GT(result_175->first.pos + result_175->second.size, 175);

    auto result_225 = tree->get_block(make_key(1, 225));
    ASSERT_TRUE(result_225.has_value());
    EXPECT_EQ(result_225->first.hash, 1);
    EXPECT_LE(result_225->first.pos, 225);
    EXPECT_GT(result_225->first.pos + result_225->second.size, 225);
}

TEST_F(BTreeGetBlockTest, SequentialBlocks) {
    // Insert sequential blocks
    auto blocks = insert_sequential_blocks(5, 1, 0, 100);

    // Test positions in each block
    for (const auto &[key, val] : blocks) {
        const uint64_t test_pos = key.pos + val.size / 2;
        auto result = tree->get_block(make_key(1, test_pos));
        ASSERT_TRUE(result.has_value()) << "Should find block for pos=" << test_pos;
        EXPECT_EQ(result->first, make_key(1, key.pos));
        EXPECT_EQ(result->second.size, val.size);
    }
}

TEST_F(BTreeGetBlockTest, RandomInsertion) {
    // Create test data and insert in random order
    auto test_blocks = create_sequential_blocks(10, 1, 0, 50);

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(test_blocks.begin(), test_blocks.end(), g);

    for (const auto &[key, val] : test_blocks) {
        tree->insert(key, val);
    }

    // Test that we can still find all blocks correctly
    for (const auto &[key, val] : test_blocks) {
        uint64_t test_pos = key.pos + val.size / 2; // Test middle of each block
        verify_get_block(key.hash, test_pos, {{key, val}});
    }
}

TEST_F(BTreeGetBlockTest, AfterInsertOperations) {
    // Start with some blocks
    insert_block(1, 100, 50);
    insert_block(1, 200, 75);

    // Verify initial state
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(1, 225, {{make_key(1, 200), make_val(1000, 75)}});

    // Insert more blocks
    insert_block(1, 300, 100);
    insert_block(2, 150, 80);

    // Verify all blocks are still findable
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(1, 225, {{make_key(1, 200), make_val(1000, 75)}});
    verify_get_block(1, 350, {{make_key(1, 300), make_val(1000, 100)}});
    verify_get_block(2, 175, {{make_key(2, 150), make_val(1000, 80)}});
}

TEST_F(BTreeGetBlockTest, AfterDeleteOperations) {
    // Insert multiple blocks
    insert_block(1, 100, 50);
    insert_block(1, 200, 75);
    insert_block(1, 300, 100);

    // Delete middle block
    tree->remove(make_key(1, 200));

    // Verify remaining blocks are still findable
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(1, 225, std::nullopt); // Deleted block
    verify_get_block(1, 350, {{make_key(1, 300), make_val(1000, 100)}});

    // Delete another block
    tree->remove(make_key(1, 100));

    // Verify only last block remains
    verify_get_block(1, 125, std::nullopt); // Deleted block
    verify_get_block(1, 350, {{make_key(1, 300), make_val(1000, 100)}});
}

TEST_F(BTreeGetBlockTest, AfterUpdateOperations) {
    // Insert a block
    insert_block(1, 100, 50);

    // Verify initial state
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});

    // Update the block to have different size
    tree_val new_val = make_val(2000, 150);
    tree->update(make_key(1, 100), new_val);

    // Verify updated block is findable with new size
    verify_get_block(1, 125, {{make_key(1, 100), new_val}});
    verify_get_block(1, 200, {{make_key(1, 100), new_val}}); // Now in range due to larger size
    verify_get_block(1, 250, std::nullopt);                  // Still outside range
}

TEST_F(BTreeGetBlockTest, LargeDataset) {
    // Insert many blocks
    insert_sequential_blocks(50, 1, 0, 100);

    // Test random positions
    std::random_device rd;
    std::mt19937 g(rd());
    std::uniform_int_distribution<uint64_t> pos_dist(0, 5000);

    for (int i = 0; i < 20; ++i) {
        uint64_t test_pos = pos_dist(g);
        auto result = tree->get_block(make_key(1, test_pos));

        if (result.has_value()) {
            // Verify the found block actually contains the test position
            EXPECT_LE(result->first.pos, test_pos);
            EXPECT_LT(test_pos, result->first.pos + result->second.size);
        }
    }
}

TEST_F(BTreeGetBlockTest, BoundaryValues) {
    // Test with boundary values
    tree_key min_key = make_key(0, 0);
    tree_key max_key = make_key(UINT64_MAX - 1, UINT64_MAX - 100);

    tree->insert(min_key, make_val(1000, 100));
    tree->insert(max_key, make_val(2000, 50));

    // Test minimum boundary
    verify_get_block(0, 0, {{min_key, make_val(1000, 100)}});
    verify_get_block(0, 50, {{min_key, make_val(1000, 100)}});
    verify_get_block(0, 100, std::nullopt);

    // Test maximum boundary
    verify_get_block(UINT64_MAX - 1, UINT64_MAX - 100, {{max_key, make_val(2000, 50)}});
    verify_get_block(UINT64_MAX - 1, UINT64_MAX - 75, {{max_key, make_val(2000, 50)}});
    verify_get_block(UINT64_MAX - 1, UINT64_MAX - 50, std::nullopt);
}

TEST_F(BTreeGetBlockTest, ComplexScenario) {
    // Create a complex scenario with multiple hashes, gaps, and operations
    insert_block(1, 100, 50);
    insert_block(1, 200, 75);
    insert_block(1, 350, 100);
    insert_block(2, 150, 80);
    insert_block(2, 300, 60);
    insert_block(3, 50, 40);

    // Perform some operations
    tree->remove(make_key(1, 200));                      // Remove middle block from hash 1
    tree->update(make_key(2, 150), make_val(5000, 120)); // Update block size
    
    // Test all remaining blocks
    verify_get_block(1, 125, {{make_key(1, 100), make_val(1000, 50)}});
    verify_get_block(1, 225, std::nullopt); // Removed block
    verify_get_block(1, 400, {{make_key(1, 350), make_val(1000, 100)}});
    
    verify_get_block(2, 200, {{make_key(2, 150), make_val(5000, 120)}}); // Updated size
    verify_get_block(2, 250, {{make_key(2, 150), make_val(5000, 120)}});
    verify_get_block(2, 330, {{make_key(2, 300), make_val(1000, 60)}});
    
    verify_get_block(3, 75, {{make_key(3, 50), make_val(1000, 40)}});
    
    // Test gaps
    verify_get_block(1, 175, std::nullopt); // Gap in hash 1
    verify_get_block(2, 299, std::nullopt); // Gap in hash 2
}
