#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "block.h"

void test_create_block(void) {
    size_t size = 1024;
    compio_block* block = compio_create_block(size, false, NULL);
    CU_ASSERT_PTR_NOT_NULL(block);
    CU_ASSERT_EQUAL(block->size, size);
    CU_ASSERT_FALSE(block->is_compressed);
    CU_ASSERT_PTR_NULL(block->compression_type);
    CU_ASSERT_PTR_NOT_NULL(block->data);
    compio_free_block(block);
}

void test_create_compressed_block(void) {
    size_t size = 2048;
    compio_block* block = compio_create_block(size, true, "gzip");
    CU_ASSERT_PTR_NOT_NULL(block);
    CU_ASSERT_EQUAL(block->size, size);
    CU_ASSERT_TRUE(block->is_compressed);
    CU_ASSERT_STRING_EQUAL(block->compression_type, "gzip");
    CU_ASSERT_PTR_NOT_NULL(block->data);
    compio_free_block(block);
}

void test_block_container(void) {
    size_t total_size = 8192;
    compio_block_container* container = compio_create_block_container(total_size);
    CU_ASSERT_PTR_NOT_NULL(container);
    CU_ASSERT_EQUAL(container->total_size, total_size);
    CU_ASSERT_EQUAL(container->block_count, 0);
    compio_free_block_container(container);
}

void test_add_block(void) {
    compio_block_container* container = compio_create_block_container(8192);
    compio_block* block = compio_create_block(1024, false, NULL);
    int result = compio_add_block(container, block);
    CU_ASSERT_EQUAL(result, 0);
    CU_ASSERT_EQUAL(container->block_count, 1);
    CU_ASSERT_PTR_EQUAL(container->blocks[0], block);
    compio_free_block_container(container);
}

void test_remove_block(void) {
    compio_block_container* container = compio_create_block_container(8192);
    compio_block* block1 = compio_create_block(1024, false, NULL);
    compio_block* block2 = compio_create_block(2048, false, NULL);
    compio_add_block(container, block1);
    compio_add_block(container, block2);
    int result = compio_remove_block(container, 0);
    CU_ASSERT_EQUAL(result, 0);
    CU_ASSERT_EQUAL(container->block_count, 1);
    CU_ASSERT_PTR_EQUAL(container->blocks[0], block2);
    compio_free_block_container(container);
}

// Function to add block tests to the test registry
void add_block_tests() {
    CU_pSuite block_suite = CU_add_suite("Block Tests", NULL, NULL);
    if (block_suite != NULL) {
        CU_add_test(block_suite, "test_create_block", test_create_block);
        CU_add_test(block_suite, "test_create_compressed_block", test_create_compressed_block);
        CU_add_test(block_suite, "test_block_container", test_block_container);
        CU_add_test(block_suite, "test_add_block", test_add_block);
        CU_add_test(block_suite, "test_remove_block", test_remove_block);
    }
}