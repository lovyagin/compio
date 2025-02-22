#include "archivers/zlib_adapter.hpp"

void ZLibAdapter::change_one_block_data(std::pair<compio::tree_key, compio::tree_val>& node,
                                        uint64_t blockChangeStart, uint64_t blockChangeSize, void* data) {
    uLongf decompressedSize = node.second.size;
    std::vector<unsigned char> decompressedBlock(decompressedSize);

    int result = uncompress(decompressedBlock.data(), &decompressedSize,
                            node.second.addr->compressedData,
                            node.second.addr->compressedDataSize);
    if (result != Z_OK) {
        std::cerr << "Decompress failed: " << zError(result) << std::endl;
        return;
    }

    uint64_t blockOffset = blockChangeStart - node.first.pos;
    memcpy(decompressedBlock.data() + blockOffset, data, blockChangeSize);

    uLongf compressedSize = compressBound(decompressedSize);
    std::vector<unsigned char> compressedData(compressedSize);
    result = compress(compressedData.data(), &compressedSize,
                      decompressedBlock.data(), decompressedSize);
    if (result != Z_OK) {
        std::cerr << "Compress failed: " << zError(result) << std::endl;
        return;
    }

    auto* compressedCopy = new unsigned char[compressedSize];
    memcpy(compressedCopy, compressedData.data(), compressedSize);

    auto* storageBlock = new StorageBlock{compressedSize, compressedCopy};
    _IbtreeP->update(node.first, {storageBlock, decompressedSize});
}

#include "archivers/zlib_adapter.hpp"

void printNodes(std::vector<std::pair<compio::tree_key, compio::tree_val>>& node) {
    for (const auto& n : node) {
        std::cout << "\nkey: [ hash: " <<  n.first.hash << ", pos: " << n.first.pos << " ]" << std::endl;
        std::cout << "val: [ addr: " << n.second.addr << ", size: " << n.second.size << " ]" << std::endl;
        std::cout << std::endl;
    }
}

void ZLibAdapter::change_data(const uint64_t hash, const uint64_t startPos, const uint64_t size, void* data) { // startPos = 3, size = 4
    std::vector<std::pair<compio::tree_key, compio::tree_val>> nodes;
    _IbtreeP->get_range({hash, startPos}, {hash, startPos + size}, nodes);

    uint64_t dataOffset = 0;
    for (auto& node : nodes) {
        uint64_t blockStart = node.first.pos;
        uint64_t blockEnd = blockStart + node.second.size;

        uint64_t changeStart = std::max(startPos, blockStart);
        uint64_t changeEnd = std::min(startPos + size, blockEnd);
        uint64_t blockChangeSize = changeEnd - changeStart;

        if (blockChangeSize == 0) continue;

        change_one_block_data(node, changeStart, blockChangeSize, static_cast<char*>(data) + dataOffset);
        dataOffset += blockChangeSize;
    }
}
