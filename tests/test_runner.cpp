#include "stdio.h"
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "test_block.h"
#include "test_btree.h"

void testChangeData();

int main() {
    CU_initialize_registry();

    add_btree_tests();
    add_block_tests();

    CU_basic_set_mode(CU_BRM_VERBOSE);

    CU_pSuite suite = CU_add_suite("ZLibAdapter Tests", nullptr, nullptr);
    CU_add_test(suite, "testChangeData", testChangeData);

    CU_basic_run_tests();

    CU_cleanup_registry();
    return 0;
}
