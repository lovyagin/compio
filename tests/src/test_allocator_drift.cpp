#include <gtest/gtest.h>
#include <vector>
#include <string>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

using namespace compio;

class AllocatorDriftTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive;
    block_allocator *allocator;
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        compio_build_default_config(&config);
        config.allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        // High threshold to disable auto-defrag unless we explicitly call it,
        // so we can test the raw allocation behavior.
        config.fragmentation_threshold = 100; 
        config.fill_holes_with_zeros = false;

        archive = compio_open_archive(fn, "w+", &config);
        allocator = archive->allocator;
    }

    void TearDown() override {
        compio_close_archive(archive);
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }
};

TEST_F(AllocatorDriftTest, SequentialDriftReusesSpace) {
    // Simulate the drift scenario:
    // 1. Allocate block A (size S)
    // 2. Allocate block B (size S)
    // 3. Free block A
    // 4. Allocate block A' (size S + small_delta)
    // Expected: A' should reuse A's space if A's size < A's space + available padding,
    // OR if A's space can be merged with subsequent free space.
    // But in a sequential write scenario, B is occupied.
    
    const size_t initial_size = 1000;
    const size_t drift_amount = 100;
    
    uint64_t addrA = allocator->allocate(initial_size);
    uint64_t addrB = allocator->allocate(initial_size);
    (void)allocator->allocate(initial_size);
    
    // Free A and B. They are adjacent.
    allocator->deallocate(addrA, initial_size);
    allocator->deallocate(addrB, initial_size);
    
    // Now we should have a free block of size 2000 at addrA.
    // Verify by allocating something larger than 1000 but smaller than 2000.
    
    uint64_t addrNew = allocator->allocate(initial_size + drift_amount);
    
    EXPECT_EQ(addrNew, addrA) << "Allocator should reuse the merged free space of A and B";
    
    // The remaining space should also be reusable.
    // Remaining: 2000 - 1100 = 900.
    uint64_t addrRemainder = allocator->allocate(500);
    // Address of new block = A + size of A' (1100)
    EXPECT_EQ(addrRemainder, addrA + initial_size + drift_amount) << "Should allocate remainder immediately after A'";
}

TEST_F(AllocatorDriftTest, SingleBlockGrowFail) {
    // Scenario: [A][B]
    // Free A.
    // Allocate A' > A.
    // Since B is occupied, A' cannot fit in A. It must go to end.
    // This is expected behavior (fragmentation), but we want to confirm it works as expected.
    
    const size_t size = 1000;
    uint64_t addrA = allocator->allocate(size);
    uint64_t addrB = allocator->allocate(size);
    
    allocator->deallocate(addrA, size);
    
    // Try to allocate slightly larger
    uint64_t addrA_Prime = allocator->allocate(size + 10);
    
    EXPECT_NE(addrA_Prime, addrA) << "Should NOT fit in A because B blocks expansion";
    // It should be allocated after B (at least B + size)
    EXPECT_GE(addrA_Prime, addrB + size) << "Should be allocated after B";
}

TEST_F(AllocatorDriftTest, MergeThreeBlocks) {
    // Scenario: [A][B][C]
    // Free A, C, then B.
    // Should merge into one big block A+B+C.
    
    const size_t size = 1000;
    uint64_t addrA = allocator->allocate(size);
    uint64_t addrB = allocator->allocate(size);
    uint64_t addrC = allocator->allocate(size);
    
    allocator->deallocate(addrA, size);
    allocator->deallocate(addrC, size);
    allocator->deallocate(addrB, size); // Middle block freed last
    
    uint64_t addrBig = allocator->allocate(size * 3);
    EXPECT_EQ(addrBig, addrA) << "Should have merged A, B, C into one contiguous block";
}

TEST_F(AllocatorDriftTest, DriftReproduction) {
    // Reproduce allocator behavior where blocks are reused, split, and then merged,
    // ensuring no unintended "drift" in file size when space is freed.
    // Scenario:
    //   Alloc A (size 1000)
    //   Alloc B (size 1000) - barrier
    //   Free A
    //   Alloc C (size 1000) -> Should reuse A (exact fit)
    //   Free C
    //   Alloc D (size 900)  -> Should reuse A, splitting it into [900][100]
    //   Free D              -> Now we have [900 free][100 free][B]
    //   Alloc E (size 1000) -> Should reuse merged [1000] block at A
    
    const size_t size = 1000;
    uint64_t addrA = allocator->allocate(size);
    uint64_t addrB = allocator->allocate(size); // Barrier
    
    allocator->deallocate(addrA, size);
    
    // 1. Exact fit reuse
    uint64_t addrC = allocator->allocate(size);
    EXPECT_EQ(addrC, addrA) << "Should reuse exact fit block";
    
    allocator->deallocate(addrC, size);
    
    // 2. Smaller fit reuse
    uint64_t addrD = allocator->allocate(size - 100);
    EXPECT_EQ(addrD, addrA) << "Should reuse larger block for smaller allocation";
    
    // Free D. Now we have [900 free] [100 free] [B]
    allocator->deallocate(addrD, size - 100);
    
    // We need to ensure the allocator merged the split block back if they are adjacent.
    // The allocator's deallocate() calls blocks_manager_.add_free_block() which calls merge_blocks().
    // So [900] and [100] should merge back to [1000].
    
    uint64_t addrE = allocator->allocate(size);
    EXPECT_EQ(addrE, addrA) << "Should reuse merged block";
}

