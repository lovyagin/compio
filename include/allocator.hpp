/**
 * @file allocator.hpp
 * @brief Tools for allocating and freeing blocks in archive
 *
 */

#ifndef COMPIO_ALLOCATOR_HEADER_
#define COMPIO_ALLOCATOR_HEADER_

#include "compio.h"
#include "compio_file.hpp"

namespace compio {

/**
 * @brief Get free block in archive of specified size
 *
 * @param archive opened archive
 * @param size size in bytes
 * @return uint64_t
 */
uint64_t allocate_block(compio_archive* archive, uint64_t size);

/**
 * @brief Free block in archive
 *
 * @param archive opened archive
 * @param addr address
 * @param size size in bytes
 */
void free_block(compio_archive* archive, uint64_t addr, uint64_t size);

} // namespace compio

#endif // COMPIO_ALLOCATOR_HEADER_