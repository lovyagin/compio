#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <stdlib.h>
#include "../include/block.h"
#include "../compio.h"

void test_compio_create_block(void) {
    printf("Creating block...\n");
    compio_block* block = compio_create_block(1024, 1);
    CU_ASSERT_PTR_NOT_NULL(block);
    if (block == NULL) {
        printf("Block creation failed: block is NULL\n");
        return;
    }
    CU_ASSERT_EQUAL(block->size, 1024);
    CU_ASSERT_EQUAL(block->is_compressed, 1);
    printf("Block created successfully\n");
    compio_free_block(block);
}

void test_compio_block_init(void) {
    int data = 123;
    printf("Initializing block...\n");
    compio_block* block = compio_block_init(0, sizeof(data), 1, &data);
    CU_ASSERT_PTR_NOT_NULL(block);
    if (block == NULL) {
        printf("Block initialization failed: block is NULL\n");
        return;
    }
    CU_ASSERT_EQUAL(block->offset, 0);
    CU_ASSERT_EQUAL(block->size, sizeof(data));
    CU_ASSERT_EQUAL(block->is_compressed, 1);
    CU_ASSERT_PTR_NOT_NULL(block->data);
    if (block->data != NULL) {
        CU_ASSERT_EQUAL(*(int*)block->data, data);
    }
    printf("Block initialized successfully\n");
    compio_free_block(block);
}

void test_compio_free_block(void) {
    printf("Creating block for free test...\n");
    compio_block* block = compio_create_block(512, 0);
    CU_ASSERT_PTR_NOT_NULL(block);
    if (block == NULL) {
        printf("Block creation failed: block is NULL\n");
        return;
    }
    printf("Block created: size=%zu, is_compressed=%d\n", block->size, block->is_compressed);
    printf("Freeing block...\n");
    compio_free_block(block);
    printf("Block freed successfully\n");
}

void test_compio_find_block_by_offset(void) {
    printf("Finding block by offset...\n");
    compio_block blocks[3] = {
            {0, 100, 0, NULL},
            {100, 200, 1, NULL},
            {300, 150, 0, NULL}
    };

    compio_block* found = compio_find_block_by_offset(blocks, 3, 100);
    CU_ASSERT_PTR_NOT_NULL(found);
    if (found == NULL) {
        printf("Block not found at offset 100\n");
    } else {
        CU_ASSERT_EQUAL(found->offset, 100);
        printf("Block found at offset 100\n");
    }

    found = compio_find_block_by_offset(blocks, 3, 250); // Does not exist
    CU_ASSERT_PTR_NULL(found);
    if (found == NULL) {
        printf("Block not found at offset 250\n");
    }
}

void test_compio_split_block_into_fragments(void) {
    printf("Splitting block into fragments...\n");
    compio_block block = {0, 1024, 0, malloc(1024)};
    size_t num_fragments;
    compio_fragment* fragments = compio_split_block_into_fragments(&block, 256, &num_fragments);

    CU_ASSERT_PTR_NOT_NULL(fragments);
    if (fragments == NULL) {
        printf("Fragmentation failed: fragments is NULL\n");
        free(block.data);
        return;
    }
    CU_ASSERT_EQUAL(num_fragments, 4);
    for (size_t i = 0; i < num_fragments; ++i) {
        CU_ASSERT_EQUAL(fragments[i].size, 256);
        CU_ASSERT_EQUAL(fragments[i].offset, i * 256);
    }
    printf("Block split into %zu fragments successfully\n", num_fragments);
    free(fragments);
    free(block.data);
}

int main(void) {
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("compio_block_tests", 0, 0);

    CU_add_test(suite, "test_compio_create_block", test_compio_create_block);
    CU_add_test(suite, "test_compio_block_init", test_compio_block_init);
    CU_add_test(suite, "test_compio_free_block", test_compio_free_block);
    CU_add_test(suite, "test_compio_find_block_by_offset", test_compio_find_block_by_offset);
    CU_add_test(suite, "test_compio_split_block_into_fragments", test_compio_split_block_into_fragments);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

    return 0;
}