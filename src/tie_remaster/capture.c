/* Developer capture: saves the flight render target as a PNG, then quits. Works with --headless. */
#include "tie_remaster/capture.h"

#include "aeron/aeron.h"
#include "aeron/time.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static float TieCapture_HalfToFloat(uint16_t h) {
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exponent = (h >> 10) & 0x1Fu;
	uint32_t mantissa = h & 0x3FFu;
	uint32_t bits;
	if (exponent == 0) {
		if (mantissa == 0) {
			bits = sign;
		} else {
			exponent = 127 - 14;
			while (!(mantissa & 0x400u)) {
				mantissa <<= 1;
				--exponent;
			}
			mantissa &= 0x3FFu;
			bits = sign | (exponent << 23) | (mantissa << 13);
		}
	} else if (exponent == 31) {
		bits = sign | 0x7F800000u | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
	}
	union {
		uint32_t u;
		float f;
	} value = { bits };
	return value.f;
}

static uint8_t TieCapture_Encode(float linear) {
	if (!(linear > 0.0f))
		return 0;
	if (linear > 1.0f)
		linear = 1.0f;
	return (uint8_t)(powf(linear, 1.0f / 2.2f) * 255.0f + 0.5f);
}

bool TieCapture_SaveRgba16F(const char* path, const uint16_t* pixels, int width, int height) {
	uint8_t* rgba = malloc((size_t)width * (size_t)height * 4u);
	if (!rgba)
		return false;
	for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
		for (int c = 0; c < 3; ++c)
			rgba[i * 4 + (size_t)c] = TieCapture_Encode(TieCapture_HalfToFloat(pixels[i * 4 + (size_t)c]));
		rgba[i * 4 + 3] = 255;
	}
	const int ok = stbi_write_png(path, width, height, 4, rgba, width * 4);
	free(rgba);
	return ok != 0;
}
