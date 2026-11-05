#include "tie_remaster/flight/cockpit/wide_png.h"

/* Static: aeron_scene already defines the stb_image implementation. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <stdlib.h>

int TieWideCockpit_ReadPng(AeronVfs* vfs, AeronVfsRoot root, const char* path, uint8_t** out_rgba,
						   int* out_width, int* out_height) {
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!vfs || !path || !AeronVfs_ReadAll(vfs, root, path, 64u * 1024u * 1024u, &bytes, &size))
		return 0;
	int channels = 0;
	uint8_t* pixels = stbi_load_from_memory(bytes, (int)size, out_width, out_height, &channels, 4);
	free(bytes);
	if (!pixels)
		return 0;
	*out_rgba = pixels;
	return 1;
}
