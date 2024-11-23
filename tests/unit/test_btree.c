#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "btree.h"

void test_btree_creation(void) {
    BTree* tree = btree_create(2);
    CU_ASSERT_PTR_NOT_NULL(tree);
    CU_ASSERT_PTR_NOT_NULL(tree->root);
    CU_ASSERT(tree->root->num_keys == 0);
    CU_ASSERT(tree->root->is_leaf == true);

    btree_free(tree);
}

void test_btree_insert_search(void) {
    BTree* tree = btree_create(2);
    btree_insert(tree, 10, "Value 10");
    btree_insert(tree, 20, "Value 20");
    btree_insert(tree, 5, "Value 5");

    CU_ASSERT_PTR_NOT_NULL(btree_search(tree->root, 10));
    CU_ASSERT_PTR_NOT_NULL(btree_search(tree->root, 20));
    CU_ASSERT_PTR_NOT_NULL(btree_search(tree->root, 5));
    CU_ASSERT_PTR_NULL(btree_search(tree->root, 15)); // Not inserted

    btree_free(tree);
}

void test_btree_remove(void) {
    BTree* tree = btree_create(2);
    btree_insert(tree, 10, "Value 10");
    btree_insert(tree, 20, "Value 20");

    CU_ASSERT_EQUAL(btree_remove(tree, 10), 0);
    CU_ASSERT_PTR_NULL(btree_search(tree->root, 10));

    CU_ASSERT_EQUAL(btree_remove(tree, 30), -1); // Nonexistent key
    btree_free(tree);
}

void add_btree_tests(void) {
    CU_pSuite suite = CU_add_suite("BTree Tests", NULL, NULL);
    CU_add_test(suite, "Test BTree Creation", test_btree_creation);
}