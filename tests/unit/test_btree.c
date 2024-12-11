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

void test_btree_find_min(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);

    // Test for empty tree
    size_t min = btree_find_min(tree);
    CU_ASSERT_EQUAL(min, 0);

    btree_insert(tree, 50, NULL);
    btree_insert(tree, 20, NULL);
    btree_insert(tree, 70, NULL);
    btree_insert(tree, 10, NULL);

    // Test for non-empty tree
    min = btree_find_min(tree);
    CU_ASSERT_EQUAL(min, 10);

    btree_free(tree);
}

void test_btree_find_max(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);

    // Test for empty tree
    size_t max = btree_find_max(tree);
    CU_ASSERT_EQUAL(max, 0);

    btree_insert(tree, 50, NULL);
    btree_insert(tree, 20, NULL);
    btree_insert(tree, 70, NULL);
    btree_insert(tree, 90, NULL);

    // Test for non-empty tree
    max = btree_find_max(tree);
    CU_ASSERT_EQUAL(max, 90);

    btree_free(tree);
}

void test_btree_update(void) {
    size_t degree = 3;
    BTree* tree = btree_create(degree);

    int value1 = 100, value2 = 200, value3 = 300;
    btree_insert(tree, 10, &value1);
    btree_insert(tree, 20, &value2);
    btree_insert(tree, 30, &value3);

    // Test updating existing key
    int new_value = 500;
    int result = btree_update(tree, 20, &new_value);
    CU_ASSERT_EQUAL(result, 0);

    // Verify the update
    BTreeNode* node = btree_search(tree, 20);
    CU_ASSERT_PTR_NOT_NULL(node);

    size_t index = 0;
    for (size_t i = 0; i < node->num_keys; i++) {
        if (node->keys[i] == 20) {
            index = i;
            break;
        }
    }

    CU_ASSERT_EQUAL(*(int*)(node->values[index]), 500);

    // Test updating non-existent key
    result = btree_update(tree, 40, &new_value);
    CU_ASSERT_EQUAL(result, -1);

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
        CU_add_test(btree_suite, "test_btree_find_min", test_btree_find_min);
        CU_add_test(btree_suite, "test_btree_find_max", test_btree_find_max);
        CU_add_test(btree_suite, "test_btree_update", test_btree_update);
    }
}
