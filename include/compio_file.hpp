#ifndef COMPIO_FILE_HEADER_
#define COMPIO_FILE_HEADER_

#include "compio.h"
#include "file.hpp"

/**
 * @brief Opened archive
 *
 */
struct compio_archive {
	FILE* file;					 /**< Opened stdio FILE */
	const compio_config* config; /**< Compio configuration */
	compio::header* header;		 /**< Read file header */

	/**
	 * @brief Parsed open mode (1 - read, 2 - write, 4 - edit, don't clear
	 * contents)
	 */
	uint8_t mode_b;
};

/**
 * @brief Opened file inside of an archive
 */
struct compio_file {
	compio_archive* archive; /**< Opened archive */
	const char* fn;			 /**< Internal filename */
	uint64_t cursor;		 /**< File cursor */
}

#endif // COMPIO_FILE_HEADER_
