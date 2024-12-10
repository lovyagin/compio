#include "allocator.hpp"
#include <cstdlib>

namespace compio {

uint64_t allocate(compio_archive* archive, uint64_t size) {
	// TODO: create table of free blocks and use it instead
	fseek(archive->file, 0, SEEK_END);
	long fsize = ftell(archive->file);
	return fsize;
}

void free(compio_archive* archive, uint64_t addr, uint64_t size) {
	// TODO: save that block into table of free blocks
	if (archive->config->fill_holes_with_zeros) {
		fseek(archive->file, addr, SEEK_SET);
		uint8_t* zeros = (uint8_t*)calloc(size, 1);
		fwrite(zeros, 1, size, archive->file);
	}
}

} // namespace compio