#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "block.h"

void test_block_creation(void) {
    compio_block* block = compio_create_block(100, false, NULL);
    CU_ASSERT_PTR_NOT_NULL(block);
    CU_ASSERT(block->size == 100);
    CU_ASSERT(block->is_compressed == false);
    CU_ASSERT_PTR_NULL(block->compression_type);
    compio_free_block(block);
}

void test_block_container(void) {
    compio_block_container* container = compio_create_block_container(1000);
    CU_ASSERT_PTR_NOT_NULL(container);

    compio_block* block1 = compio_create_block(100, false, NULL);
    compio_block* block2 = compio_create_block(200, true, "gzip");

    block1->offset = 0;
    block2->offset = 100;

    CU_ASSERT_EQUAL(compio_add_block(container, block1), 0);
    CU_ASSERT_EQUAL(compio_add_block(container, block2), 0);

    size_t block_offset;
    compio_block* found_block = compio_find_block(container, 50, &block_offset);
    CU_ASSERT_PTR_NOT_NULL(found_block);
    CU_ASSERT_EQUAL(found_block, block1);

    found_block = compio_find_block(container, 150, &block_offset);
    CU_ASSERT_PTR_NOT_NULL(found_block);
    CU_ASSERT_EQUAL(found_block, block2);

    CU_ASSERT_EQUAL(compio_remove_block(container, 0), 0);
    CU_ASSERT_PTR_NULL(compio_find_block(container, 50, &block_offset));

    compio_free_block_container(container);
}

void add_block_tests(void) {
    CU_pSuite suite = CU_add_suite("Block Tests", NULL, NULL);
    CU_add_test(suite, "Test Block Creation", test_block_creation);
}