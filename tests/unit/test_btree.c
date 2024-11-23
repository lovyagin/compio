#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "btree.h"

void test_create_btree(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    CU_ASSERT_PTR_NOT_NULL(tree);
    CU_ASSERT_PTR_NOT_NULL(tree->root);
    CU_ASSERT_EQUAL(tree->degree, degree);
    CU_ASSERT_EQUAL(tree->root->num_keys, 0);
    CU_ASSERT_TRUE(tree->root->is_leaf);
    btree_free(tree);
}

void test_btree_insert(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    btree_insert(tree, 10, NULL);
    CU_ASSERT_EQUAL(tree->root->num_keys, 1);
    CU_ASSERT_EQUAL(tree->root->keys[0], 10);
    btree_insert(tree, 20, NULL);
    CU_ASSERT_EQUAL(tree->root->num_keys, 2);
    CU_ASSERT_EQUAL(tree->root->keys[1], 20);
    btree_insert(tree, 5, NULL);
    CU_ASSERT_EQUAL(tree->root->num_keys, 3);
    CU_ASSERT_EQUAL(tree->root->keys[0], 5);
    CU_ASSERT_EQUAL(tree->root->keys[1], 10);
    CU_ASSERT_EQUAL(tree->root->keys[2], 20);
    btree_free(tree);
}

void test_btree_split(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    btree_insert(tree, 10, NULL);
    btree_insert(tree, 20, NULL);
    btree_insert(tree, 5, NULL);
    btree_insert(tree, 6, NULL);
    btree_insert(tree, 12, NULL);
    btree_insert(tree, 30, NULL);
    btree_insert(tree, 7, NULL);
    btree_insert(tree, 17, NULL);

    CU_ASSERT_EQUAL(tree->root->num_keys, 1);
    CU_ASSERT_EQUAL(tree->root->keys[0], 10);
    CU_ASSERT_EQUAL(tree->root->children[0]->num_keys, 3);
    CU_ASSERT_EQUAL(tree->root->children[1]->num_keys, 4);
    btree_free(tree);
}

void test_btree_delete(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    btree_insert(tree, 10, NULL);
    btree_insert(tree, 20, NULL);
    btree_insert(tree, 5, NULL);
    btree_insert(tree, 6, NULL);
    btree_insert(tree, 12, NULL);
    btree_insert(tree, 30, NULL);
    btree_insert(tree, 7, NULL);
    btree_insert(tree, 17, NULL);
    
    btree_delete(tree, 6);
    CU_ASSERT_PTR_NOT_NULL(tree);  // Ensure tree still exists

    btree_free(tree);
}

void test_btree_search(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    btree_insert(tree, 10, NULL);
    btree_insert(tree, 20, NULL);
    btree_insert(tree, 5, NULL);
    btree_insert(tree, 6, NULL);
    btree_insert(tree, 12, NULL);
    btree_insert(tree, 30, NULL);
    btree_insert(tree, 7, NULL);
    btree_insert(tree, 17, NULL);

    // Searching for existing keys
    BTreeNode* found_node = btree_search(tree, 6);
    CU_ASSERT_PTR_NOT_NULL(found_node);
    CU_ASSERT_EQUAL(found_node->keys[1], 6);

    // Searching for a non-existing key
    found_node = btree_search(tree, 100);
    CU_ASSERT_PTR_NULL(found_node);

    btree_free(tree);
}

void test_btree_large_insert(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);
    for (size_t i = 1; i <= 50; i++) {
        btree_insert(tree, i, NULL);
    }
    // Check if root has correct number of keys after multiple inserts
    CU_ASSERT_PTR_NOT_NULL(tree->root);
    CU_ASSERT_TRUE(tree->root->num_keys > 0);
    CU_ASSERT_TRUE(tree->root->num_keys <= (2 * degree - 1));
    btree_free(tree);
}

// Function to add BTree tests to the test registry
void add_btree_tests() {
    CU_pSuite btree_suite = CU_add_suite("BTree Tests", NULL, NULL);
    if (btree_suite != NULL) {
        CU_add_test(btree_suite, "test_create_btree", test_create_btree);
        CU_add_test(btree_suite, "test_btree_insert", test_btree_insert);
        CU_add_test(btree_suite, "test_btree_split", test_btree_split);
        CU_add_test(btree_suite, "test_btree_delete", test_btree_delete);
        CU_add_test(btree_suite, "test_btree_search", test_btree_search);
        CU_add_test(btree_suite, "test_btree_large_insert", test_btree_large_insert);
    }
}
