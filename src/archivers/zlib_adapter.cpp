#include "archivers/zlib_adapter.hpp"

void ZLibAdapter::change_one_block_data(std::pair<compio::tree_key, compio::tree_val>& node, uint64_t startPos, uint64_t size, void* data) {
    auto* decompressedBlock = static_cast<unsigned char *>(malloc(node.second.size));
    int result = uncompress(
        decompressedBlock,
        &node.second.size,
        node.second.addr->compressedData,
        node.second.addr->compressedDataSize);


    if (result != Z_OK) {
        std::cout << "decompress failed: " << zError(result) << std::endl;
    }

    auto* startPosForChange = decompressedBlock + (startPos - node.first.pos);
    std::memcpy(startPosForChange, data, size);

    ulong compressedSize = compressBound(sizeof(decompressedBlock));
    auto* compressedData = static_cast<unsigned char *>(malloc(compressedSize));
    result = compress(compressedData, &compressedSize, decompressedBlock, node.second.size);

    if (result != Z_OK) {
        std::cout << "compress failed: " << zError(result) << std::endl;
    }

    free(decompressedBlock);

    auto* storageBlock = new StorageBlock{compressedSize, compressedData}; // TODO: ADD TO STORAGE BLOCK, CREATE DELETE,
                                                                           // NOW MEMORY IS LEAKING
    _IbtreeP->update(node.first, {storageBlock, node.second.size}); // TODO: CREATE MOVE FOR NODE IF TO BIG
}

void ZLibAdapter::change_data(const uint64_t hash, const uint64_t startPos, const uint64_t size, void* data) {
    std::vector<std::pair<compio::tree_key, compio::tree_val>> nodes;
    _IbtreeP->get_range({hash, startPos}, {hash, startPos + size}, nodes);

    if (nodes.empty()) return; 

    uint64_t dataOffset = 0;
    for (auto& node : nodes) {
        const uint64_t blockChangeStart = std::max(startPos, node.first.pos);
        const uint64_t blockChangeSize = std::min(node.second.size, size);
        change_one_block_data(node, blockChangeStart, blockChangeSize, data + dataOffset);
        dataOffset += blockChangeSize;
    }

}
