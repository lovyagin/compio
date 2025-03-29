#include "allocator.hpp"
#include "cstdbool"
#include "malloc.h"
#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <stdio.h>

#include <block.h>

// Mock structures for testing
typedef struct {
    uint64_t file_size;
} header;

typedef struct {
    allocation_strategy allocation_strategy;
    uint8_t fragmentation_threshold;
    bool fill_holes_with_zeros;
} compio_config;

typedef struct {
    header* header;
    compio_config* config;
    FILE* file;
    void* index;
} compio_archive;

// Test FIRST_FIT strategy
void test_first_fit_allocation(void) {
    compio_archive archive = {0};
    archive.config = malloc(sizeof(compio_config));
    archive.config->allocation_strategy = FIRST_FIT;
    archive.config->fragmentation_threshold = 50;
    archive.header = malloc(sizeof(header));
    archive.header->file_size = sizeof(header);

    compio::block_allocator allocator(&archive);
    
    // Add free blocks: [100-300), [400-500)
    allocator.blocks_manager_.add_free_block(100, 200);
    allocator.blocks_manager_.add_free_block(400, 100);

    // Allocate 150 bytes
    uint64_t offset = allocator.allocate(150);
    CU_ASSERT_EQUAL(offset, 100);
    CU_ASSERT_EQUAL(allocator.blocks_manager_.total_free_, 150);

    free(archive.config);
    free(archive.header);
}

// Test block merging logic
void test_free_block_merging(void) {
    compio::free_blocks_manager manager(nullptr);
    
    // Add adjacent blocks
    manager.add_free_block(0, 100);
    manager.add_free_block(100, 100);
    
    CU_ASSERT_EQUAL(manager.head_->size, 200);
    CU_ASSERT_EQUAL(manager.total_free_, 200);

    // Add non-adjacent block
    manager.add_free_block(300, 100);
    CU_ASSERT_EQUAL(manager.head_->size, 200);
    CU_ASSERT_EQUAL(manager.tail_->size, 100);
}

// Test defragmentation threshold
void test_defragmentation_trigger(void) {
    compio_archive archive = {0};
    archive.config = malloc(sizeof(compio_config));
    archive.config->fragmentation_threshold = 30;
    archive.header = malloc(sizeof(header));
    archive.header->file_size = sizeof(header);

    compio::block_allocator allocator(&archive);
    
    // Create fragmented layout
    allocator.blocks_manager_.add_free_block(0, 50);
    allocator.blocks_manager_.add_free_block(100, 50);
    allocator.blocks_manager_.add_free_block(200, 50);

    // Fragmentation should be ~66%
    allocator.maintenance();
    CU_ASSERT(allocator.needs_defragmentation());

    free(archive.config);
    free(archive.header);
}

// Test compressed block integration
void test_compressed_block_handling(void) {
    compio_block* block = compio_create_block(1024, true, "lz4");
    compio_block_container* container = compio_create_block_container(8192);
    
    CU_ASSERT_EQUAL(compio_add_block(container, block), 0);
    CU_ASSERT_EQUAL(container->block_count, 1);
    CU_ASSERT_TRUE(block->is_compressed);
    
    compio_free_block_container(container);
}

// Error handling test
void test_allocation_failure(void) {
    compio_archive archive = {0};
    archive.config = malloc(sizeof(compio_config));
    archive.config->allocation_strategy = FIRST_FIT;
    archive.header = malloc(sizeof(header));
    archive.header->file_size = 1000;  // Limited space

    compio::block_allocator allocator(&archive);
    
    uint64_t offset = allocator.allocate(2000);
    CU_ASSERT_EQUAL(offset, UINT64_MAX);

    free(archive.config);
    free(archive.header);
}

// Add tests to suite
void add_allocator_tests() {
    CU_pSuite suite = CU_add_suite("Allocator Tests", NULL, NULL);
    if (suite != NULL) {
        CU_add_test(suite, "FIRST_FIT allocation", test_first_fit_allocation);
        CU_add_test(suite, "Free block merging", test_free_block_merging);
        CU_add_test(suite, "Defragmentation trigger", test_defragmentation_trigger);
        CU_add_test(suite, "Compressed block handling", test_compressed_block_handling);
        CU_add_test(suite, "Allocation failure", test_allocation_failure);
    }
}
[file content end]