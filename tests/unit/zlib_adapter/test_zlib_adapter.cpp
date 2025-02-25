#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <zlib.h>

#include "include/archivers/zlib_adapter.hpp"
#include "tests/unit/include/mockBTree/MockBTree.hpp"

unsigned char testData[] = {0x01, 0x02, 0x03, 0x04, 0x05};
unsigned char compressedData[1024];
ulong compressedSize = sizeof(compressedData);

void testChangeData() {
    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key, val});
    mockBTreeP->insert_segment(123, {0, 10});

    int result = compress(compressedData, &compressedSize, testData, sizeof(testData));
    CU_ASSERT_EQUAL(result, Z_OK);

    unsigned char newData[] = {0x06, 0x07};
    adapter.change_data(123, 1, 2, newData);

    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 1);
    auto& updatedNode = mockBTreeP->nodes[0];
    CU_ASSERT_EQUAL(updatedNode.val.size, 5);

    unsigned char decompressedData[1024];
    ulong decompressedSize = sizeof(decompressedData);
    result = uncompress(decompressedData, &decompressedSize, updatedNode.val.addr->compressedData, updatedNode.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData[1], 0x06);
    CU_ASSERT_EQUAL(decompressedData[2], 0x07);
}
