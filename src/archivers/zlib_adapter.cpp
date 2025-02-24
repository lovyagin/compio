//
// Адаптер для работы с zlib
//

#include "archivers/zlib_adapter.hpp"


void ZLibAdapter::change_data(const uint64_t hash, const uint64_t startPos, const uint64_t size, void* data) {
    std::vector<std::pair<compio::tree_key, compio::tree_val>> result;
    _btreeP->get_range({hash, startPos}, {hash, startPos + size}, result);

    if (result.empty() || result.size() > 1) return; // TODO: implement two and more blocks

    auto node = result[0];

    // TODO: DECOMPRESS
    auto decompressedBlock = node.second;

    StorageBlock* startPosForChange = decompressedBlock.addr + (startPos - node.first.pos);
    // auto targetAddress = reinterpret_cast<void*>(startPosForChange); // bad for 32-bit systems??
    std::memcpy(startPosForChange, data, size);

    // TODO: COMPRESS AND UPDATE B-TREE
}
