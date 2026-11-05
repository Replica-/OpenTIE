#ifndef TIE_REMASTER_CAPTURE_H
#define TIE_REMASTER_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/* Tone-maps an RGBA16F image to 8-bit sRGB-ish PNG. */
bool TieCapture_SaveRgba16F(const char* path, const uint16_t* pixels, int width, int height);

#endif
