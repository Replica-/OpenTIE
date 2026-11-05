#ifndef TIE_REMASTER_COCKPIT_WIDE_PNG_H
#define TIE_REMASTER_COCKPIT_WIDE_PNG_H

#include "aeron/vfs.h"

#include <stdint.h>

/* Reads and decodes a PNG into straight-alpha RGBA8 (caller frees with free()). Returns 0 if the file
 * is missing or is not a valid image. */
int TieWideCockpit_ReadPng(AeronVfs* vfs, AeronVfsRoot root, const char* path, uint8_t** out_rgba,
						   int* out_width, int* out_height);

#endif
