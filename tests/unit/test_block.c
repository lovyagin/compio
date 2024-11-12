#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "../include/block.h"

void test_compio_create_block() {
    compio_block* block = compio_create_block(1024, 1);
    CU_ASSERT_PTR_NOT_NULL(block);
    CU_ASSERT_EQUAL(block->size, 1024);
    CU_ASSERT_EQUAL(block->is_compressed, 1);
    compio_free_block(block);
}

void test_compio_block_init() {
    int data = 123;
    compio_block* block = compio_block_init(0, 1024, 1, &data);
    CU_ASSERT_PTR_NOT_NULL(block);
    CU_ASSERT_EQUAL(block->offset, 0);
    CU_ASSERT_EQUAL(block->size, 1024);
    CU_ASSERT_EQUAL(block->is_compressed, 1);
    CU_ASSERT_PTR_EQUAL(block->data, &data);
    compio_free_block(block);
}

void test_compio_free_block() {
    compio_block* block = compio_create_block(512, 0);
    CU_ASSERT_PTR_NOT_NULL(block);
    compio_free_block(block);
    // Мы не можем напрямую проверить освобождение памяти,
    // но отсутствие ошибок считается успешным результатом
}

int main() {
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("compio_block_tests", 0, 0);

    CU_add_test(suite, "test_compio_create_block", test_compio_create_block);
    CU_add_test(suite, "test_compio_block_init", test_compio_block_init);
    CU_add_test(suite, "test_compio_free_block", test_compio_free_block);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

    return 0;
}
