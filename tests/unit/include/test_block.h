#ifndef TEST_BLOCK_H
#define TEST_BLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "block.h"

void test_create_block(void);
void test_create_compressed_block(void);
void test_block_container(void);
void test_add_block(void);
void test_remove_block(void);
void test_find_block(void);
void test_find_block_by_offset(void);
void add_block_tests();

#ifdef __cplusplus
}
#endif

#endif //TEST_BLOCK_H
