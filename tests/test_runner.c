#include "stdio.h"
#ifdef _MSC_VER
#undef snprintf
#endif

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

void add_btree_tests();
void add_block_tests();

int main() {
    CU_initialize_registry();

    add_btree_tests();
    add_block_tests();

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();

    CU_cleanup_registry();
    return 0;
}
