#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "file.hpp"

using namespace compio;

header::header() : magic_number(0), index_root(0), original_fsize(0) {}

auto read_header(FILE* file) {
	auto result = std::make_shared<header>(new header());
	fseek(file, SEEK_SET, 0);
	int bytes = fread(result.get(), sizeof(header), 1, file);
    if (bytes < sizeof(header))
        throw std::runtime_error("Failed to read header from file");
	return result;
}

auto read_index_node(FILE* file, uint64_t addr, int tree_order) {
	auto result = std::make_shared<index_node>(new index_node());
	fseek(file, SEEK_SET, addr);

	// size of metadata (independent of tree_order)
	int const_data_size = offsetof(index_node, keys);
	// total bytes in file
	int total_size = const_data_size + tree_order * 3 * sizeof(uint64_t);

	char* buf = new char[total_size];

	int bytes = fread(buf, total_size, 1, file);
    if (bytes < total_size)
        throw std::runtime_error("Failed to read index node from file");
	memcpy(result.get(), buf, const_data_size);

	// pointer to already allocated buf
	result->keys = (uint64_t*)(buf + const_data_size);
	result->blocks = result->keys + tree_order;
	result->children = result->blocks + tree_order;

	return result;
}

index_node::~index_node() {
	// delete previously allocated buffer
	int const_data_size = sizeof(uint8_t) + sizeof(uint32_t);
	char* buf = (char*)keys - const_data_size;
	delete buf;
}

auto read_storage_block(FILE* file, uint64_t addr) {
    auto result = std::make_shared<storage_block>(new storage_block());
	fseek(file, SEEK_SET, addr);

    // size of metadata (independent of tree_order)
	int const_data_size = offsetof(storage_block, data);
    int bytes = fread(result.get(), const_data_size, 1, file);
    if (bytes < const_data_size)
        throw std::runtime_error("Failed to read storage block metadata from file");
    // read data, as we already know its size
    result->data = new uint8_t[result->size];
    bytes = fread(result->data, result->size, 1, file);
    if (bytes < result->size)
        throw std::runtime_error("Failed to read storage block data from file");
	
	return result;
}

storage_block::~storage_block() {
    delete data;
}