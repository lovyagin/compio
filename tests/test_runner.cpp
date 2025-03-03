#include "stdio.h"
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "test_block.h"
#include "test_btree.h"

void testChangeDataOneBlock();
void testChangeDataTwoBlocks();
void testChangeDataAtBlockEdges();
void testChangeDataOverflow();
void testBTreeMetadataUpdate();
void testChangeDataNoBlocks();
void testChangeDataThreeBlocks();

int main() {
    CU_initialize_registry();

    add_btree_tests();
    add_block_tests();

    CU_basic_set_mode(CU_BRM_VERBOSE);

    CU_pSuite suite = CU_add_suite("ZLibAdapter Tests", nullptr, nullptr);
    CU_add_test(suite, "testChangeDataOneBlock", testChangeDataOneBlock);
    CU_add_test(suite, "testChangeDataTwoBlocks", testChangeDataTwoBlocks);
    CU_add_test(suite, "testChangeDataAtBlockEdges", testChangeDataAtBlockEdges);
    CU_add_test(suite, "testChangeDataNoBlocks", testChangeDataNoBlocks);
    CU_add_test(suite, "testChangeDataOverflow", testChangeDataOverflow);
    CU_add_test(suite, "testBTreeMetadataUpdate", testBTreeMetadataUpdate);
    // CU_add_test(suite, "testChangeDataThreeBlocks", testChangeDataThreeBlocks); // TODO: FIX


    CU_basic_run_tests();

    CU_cleanup_registry();
    return 0;
}
