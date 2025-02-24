#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <zlib.h>

#include "include/archivers/zlib_adapter.hpp"
#include "tests/unit/include/mockBTree/MockBTree.hpp"

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
    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    std::cout << "[test]: created adapter" << std::endl;
    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 5};
    mockBTreeP->nodes.emplace_back(key, val);
    mockBTreeP->insert_segment(123, {0, 10});

    std::cout << "[test]: created node" << std::endl;

    int result = compress(compressedData, &compressedSize, testData, sizeof(testData));
    CU_ASSERT_EQUAL(result, Z_OK);

    std::cout << "[test]: COMPRESSED OK, compressed size: " << compressedSize << std::endl;

    unsigned char newData[] = {0x06, 0x07};
    adapter.change_data(123, 1, 2, newData);

    std::cout << "[test]: changed data" << std::endl;
    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 1);
    auto& updatedNode = mockBTreeP->nodes[0];
    CU_ASSERT_EQUAL(updatedNode.second.size, 5);

    std::cout << "[test]: data check" << std::endl;

    unsigned char decompressedData[1024];
    ulong decompressedSize = sizeof(decompressedData);
    result = uncompress(decompressedData, &decompressedSize, updatedNode.second.addr->compressedData, updatedNode.second.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);

    std::cout << "[test]: unpacked data" << std::endl;

    std::cout <<  "[test]: decompressedData[1]: " << static_cast<int>(decompressedData[1]) << std::endl;
    std::cout <<  "[test]: decompressedData[2]: " << static_cast<int>(decompressedData[2]) << std::endl;
    CU_ASSERT_EQUAL(decompressedData[1], 0x06);
    CU_ASSERT_EQUAL(decompressedData[2], 0x07);

    std::cout << "[test]: data check" << std::endl;
}
