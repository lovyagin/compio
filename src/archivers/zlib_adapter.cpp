//
// Адаптер для работы с zlib
//

#include "archivers/zlib_adapter.hpp"


void ZLibAdapter::change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) {
    std::vector<std::pair<compio::tree_key, compio::tree_val>> result;
    _btreeP->get_range({hash, startPos}, {hash, startPos + size}, result);

    if (result.empty() || result.size() > 1) return; // TODO: implement two and more blocks

    auto node = result[0];

    // TODO: DECOMPRESS
    auto decompressedBlock = node.second;

    uint64_t startPosForChange = decompressedBlock.addr + (size - startPos);
    void* targetAddress = reinterpret_cast<void*>(startPosForChange);
    std::memcpy(targetAddress, data, size);

    // TODO: COMPRESS
}
