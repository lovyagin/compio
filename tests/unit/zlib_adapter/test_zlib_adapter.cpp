#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <zlib.h>

#include "include/archivers/zlib_adapter.hpp"
#include "tests/unit/include/mockBTree/MockBTree.hpp"

void testChangeDataOneBlock() {
    unsigned char testData[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char compressedData[1024];
    ulong compressedSize = sizeof(compressedData);

    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key, val});
    mockBTreeP->insert_segment(123, {0, 5});

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

void testChangeDataTwoBlocks() {
    unsigned char testData1[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char testData2[] = {0x06, 0x07, 0x08, 0x09, 0x0A};

    unsigned char compressedData1[1024], compressedData2[1024];
    ulong compressedSize1 = sizeof(compressedData1);
    ulong compressedSize2 = sizeof(compressedData2);

    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    int result = compress(compressedData1, &compressedSize1, testData1, sizeof(testData1));
    CU_ASSERT_EQUAL(result, Z_OK);

    int result2 = compress(compressedData2, &compressedSize2, testData2, sizeof(testData2));
    CU_ASSERT_EQUAL(result2, Z_OK);

    compio::tree_key key1 = {123, 0};
    auto* block1 = new StorageBlock{compressedSize1, new unsigned char[compressedSize1]};
    memcpy(block1->compressedData, compressedData1, compressedSize1);
    compio::tree_val val1 = {block1, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key1, val1});
    mockBTreeP->insert_segment(123, {0, 4});

    compio::tree_key key2 = {123, 5};
    auto* block2 = new StorageBlock{compressedSize2, new unsigned char[compressedSize2]};
    memcpy(block2->compressedData, compressedData2, compressedSize2);
    compio::tree_val val2 = {block2, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key2, val2});
    mockBTreeP->insert_segment(123, {5, 9});

    unsigned char newData[] = {0xAA, 0xBB, 0xCC, 0xDD};
    adapter.change_data(123, 3, 4, newData);

    auto& updatedNode1 = mockBTreeP->nodes[0];
    unsigned char decompressedData1[1024];
    uLongf decompressedSize1 = sizeof(decompressedData1);
    result = uncompress(decompressedData1, &decompressedSize1, updatedNode1.val.addr->compressedData, updatedNode1.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData1[3], 0xAA);
    CU_ASSERT_EQUAL(decompressedData1[4], 0xBB);

    auto& updatedNode2 = mockBTreeP->nodes[1];
    unsigned char decompressedData2[1024];
    uLongf decompressedSize2 = sizeof(decompressedData2);
    result = uncompress(decompressedData2, &decompressedSize2, updatedNode2.val.addr->compressedData, updatedNode2.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData2[0], 0xCC);
    CU_ASSERT_EQUAL(decompressedData2[1], 0xDD); 
}

