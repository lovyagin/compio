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

void testChangeDataAtBlockEdges() {
    unsigned char testData[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char compressedData[1024];
    ulong compressedSize = sizeof(compressedData);

    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key, val});
    mockBTreeP->insert_segment(123, {0, 4});

    int result = compress(compressedData, &compressedSize, testData, sizeof(testData));
    CU_ASSERT_EQUAL(result, Z_OK);

    unsigned char newData[] = {0xFF, 0xFE};
    adapter.change_data(123, 0, 1, newData);
    adapter.change_data(123, 4, 1, newData + 1);

    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 1);

    auto& updatedNode = mockBTreeP->nodes[0];
    unsigned char decompressedData[1024];
    uLongf decompressedSize = sizeof(decompressedData);
    result = uncompress(decompressedData, &decompressedSize, updatedNode.val.addr->compressedData, updatedNode.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData[0], 0xFF);
    CU_ASSERT_EQUAL(decompressedData[4], 0xFE);
}

void testChangeDataOverflow() {
    unsigned char testData[] = {0x01, 0x02, 0x03};
    unsigned char compressedData[1024];
    ulong compressedSize = sizeof(compressedData);

    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    compio::tree_key key = {123, 0};
    compio::tree_val val = {new StorageBlock{compressedSize, compressedData}, 3};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key, val});
    mockBTreeP->insert_segment(123, {0, 2});

    int result = compress(compressedData, &compressedSize, testData, sizeof(testData));
    CU_ASSERT_EQUAL(result, Z_OK);

    unsigned char newData[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    adapter.change_data(123, 0, 5, newData);

    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 1);

    auto& updatedNode = mockBTreeP->nodes[0];
    unsigned char decompressedData[1024];
    uLongf decompressedSize = sizeof(decompressedData);
    result = uncompress(decompressedData, &decompressedSize, updatedNode.val.addr->compressedData, updatedNode.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData[0], 0xAA);
    CU_ASSERT_EQUAL(decompressedData[1], 0xBB);
    CU_ASSERT_EQUAL(decompressedData[2], 0xCC);
}

void testBTreeMetadataUpdate() {
    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    compio::tree_key key1 = {123, 0};
    compio::tree_val val1 = {new StorageBlock{0, nullptr}, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key1, val1});

    compio::tree_key key2 = {123, 5};
    compio::tree_val val2 = {new StorageBlock{0, nullptr}, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key2, val2});

    unsigned char newData[] = {0xAA, 0xBB};
    adapter.change_data(123, 3, 2, newData);

    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 2);

    CU_ASSERT_EQUAL(mockBTreeP->nodes[0].key.pos, 0);
    CU_ASSERT_EQUAL(mockBTreeP->nodes[1].key.pos, 5);
}

void testChangeDataNoBlocks() {
    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    unsigned char newData[] = {0xAA, 0xBB};
    adapter.change_data(123, 0, 2, newData); // Нет блоков

    CU_ASSERT_EQUAL(mockBTreeP->nodes.size(), 0);
}

void testChangeDataThreeBlocks() {
    // Исходные данные для трех блоков
    unsigned char testData1[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char testData2[] = {0x06, 0x07, 0x08, 0x09, 0x0A};
    unsigned char testData3[] = {0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

    unsigned char compressedData1[1024], compressedData2[1024], compressedData3[1024];
    ulong compressedSize1 = sizeof(compressedData1);
    ulong compressedSize2 = sizeof(compressedData2);
    ulong compressedSize3 = sizeof(compressedData3);

    auto mockBTreeP = std::make_shared<MockBTree>();
    ZLibAdapter adapter{mockBTreeP};

    // Сжатие трех блоков
    int result = compress(compressedData1, &compressedSize1, testData1, sizeof(testData1));
    CU_ASSERT_EQUAL(result, Z_OK);

    int result2 = compress(compressedData2, &compressedSize2, testData2, sizeof(testData2));
    CU_ASSERT_EQUAL(result2, Z_OK);

    int result3 = compress(compressedData3, &compressedSize3, testData3, sizeof(testData3));
    CU_ASSERT_EQUAL(result3, Z_OK);

    // Первый блок (позиции 0-4)
    compio::tree_key key1 = {123, 0};
    auto* block1 = new StorageBlock{compressedSize1, new unsigned char[compressedSize1]};
    memcpy(block1->compressedData, compressedData1, compressedSize1);
    compio::tree_val val1 = {block1, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key1, val1});
    mockBTreeP->insert_segment(123, {0, 4});

    // Второй блок (позиции 5-9)
    compio::tree_key key2 = {123, 5};
    auto* block2 = new StorageBlock{compressedSize2, new unsigned char[compressedSize2]};
    memcpy(block2->compressedData, compressedData2, compressedSize2);
    compio::tree_val val2 = {block2, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key2, val2});
    mockBTreeP->insert_segment(123, {5, 9});

    // Третий блок (позиции 10-14)
    compio::tree_key key3 = {123, 10};
    auto* block3 = new StorageBlock{compressedSize3, new unsigned char[compressedSize3]};
    memcpy(block3->compressedData, compressedData3, compressedSize3);
    compio::tree_val val3 = {block3, 5};
    mockBTreeP->nodes.emplace_back(MockBTree::KeyValuePair{key3, val3});
    mockBTreeP->insert_segment(123, {10, 14});

    // Изменение данных с позиции 3, размер 6 байт (захватывает 3 блока)
    unsigned char newData[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    adapter.change_data(123, 3, 6, newData);

    // Проверка первого блока (измененные байты 3-4)
    auto& updatedNode1 = mockBTreeP->nodes[0];
    unsigned char decompressedData1[1024];
    uLongf decompressedSize1 = sizeof(decompressedData1);
    result = uncompress(decompressedData1, &decompressedSize1, updatedNode1.val.addr->compressedData, updatedNode1.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData1[3], 0xAA);
    CU_ASSERT_EQUAL(decompressedData1[4], 0xBB);

    // Проверка второго блока (измененные байты 0-4)
    auto& updatedNode2 = mockBTreeP->nodes[1];
    unsigned char decompressedData2[1024];
    uLongf decompressedSize2 = sizeof(decompressedData2);
    result = uncompress(decompressedData2, &decompressedSize2, updatedNode2.val.addr->compressedData, updatedNode2.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData2[0], 0xCC);
    CU_ASSERT_EQUAL(decompressedData2[1], 0xDD);
    CU_ASSERT_EQUAL(decompressedData2[2], 0xEE);
    CU_ASSERT_EQUAL(decompressedData2[3], 0xFF);
    CU_ASSERT_EQUAL(decompressedData2[4], 0x0A);  // неизмененный байт

    // Проверка третьего блока (только первый байт изменен)
    auto& updatedNode3 = mockBTreeP->nodes[2];
    unsigned char decompressedData3[1024];
    uLongf decompressedSize3 = sizeof(decompressedData3);
    result = uncompress(decompressedData3, &decompressedSize3, updatedNode3.val.addr->compressedData, updatedNode3.val.addr->compressedDataSize);
    CU_ASSERT_EQUAL(result, Z_OK);
    CU_ASSERT_EQUAL(decompressedData3[0], 0x0B);  // неизмененный байт
    CU_ASSERT_EQUAL(decompressedData3[1], 0x0C);
    CU_ASSERT_EQUAL(decompressedData3[2], 0x0D);
    CU_ASSERT_EQUAL(decompressedData3[3], 0x0E);
    CU_ASSERT_EQUAL(decompressedData3[4], 0x0F);
}

