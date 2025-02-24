#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <vector>
#include <cstring>
#include <iostream>
#include "include/archivers/zlib_adapter.hpp"
#include "tests/unit/include/mockBTree/MockBTree.h"

// Mock для StorageBlock
//struct StorageBlock {
//    ulong compressedDataSize;
//    unsigned char* compressedData;
//
//    ~StorageBlock() {
//        free(compressedData);
//    }
//};

// Тестовые данные
unsigned char testData[] = {0x01, 0x02, 0x03, 0x04, 0x05};
unsigned char compressedData[1024];
ulong compressedSize = sizeof(compressedData);

// Инициализация тестов
void testChangeData() {
    MockBTree mockBTree;
    ZLibAdapter adapter(std::make_unique<mockBTree>());

    // Подготовка тестовых данных
    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 5};
    mockBTree.nodes.push_back({key, val});

    // Сжатие тестовых данных
    int result = compress(compressedData, &compressedSize, testData, sizeof(testData));
    CU_ASSERT_EQUAL(result, Z_OK);

    // Вызов тестируемой функции
    unsigned char newData[] = {0x06, 0x07};
    adapter.change_data(123, 1, 2, newData);

    // Проверка результата
    CU_ASSERT_EQUAL(mockBTree.nodes.size(), 1);
    auto& updatedNode = mockBTree.nodes[0];
    CU_ASSERT_EQUAL(updatedNode.second.size, 5);

    // Распаковка данных для проверки
    unsigned char decompressedData[1024];
    ulong decompressedSize = sizeof(decompressedData);
    result = uncompress(decompressedData, &decompressedSize, updatedNode.second.addr->compressedData, updatedNode.second.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);

    // Проверка измененных данных
    CU_ASSERT_EQUAL(decompressedData[1], 0x06);
    CU_ASSERT_EQUAL(decompressedData[2], 0x07);
}

// Регистрация тестов
int main() {
    CU_initialize_registry();

    CU_pSuite suite = CU_add_suite("ZLibAdapter Tests", nullptr, nullptr);
    CU_add_test(suite, "testChangeData", testChangeData);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

    return 0;
}