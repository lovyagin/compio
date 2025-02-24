#ifndef TEST_BTREE_H
#define TEST_BTREE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "BTree/btree.h"

void test_btree_insert(void);
void test_btree_split(void);
void test_btree_delete(void);
void test_btree_search(void);
void test_btree_large_insert(void);
void test_btree_find_min(void);
void test_btree_find_max(void);
void test_btree_update(void);
void add_btree_tests();

#ifdef __cplusplus
}
#endif

#endif //TEST_BTREE_H
