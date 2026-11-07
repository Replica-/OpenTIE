#ifndef TIE_FESTRING_H
#define TIE_FESTRING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TIE98 widened the cursor coordinates to int. */
#if defined(TIE98) && !defined(TIE_MODERN)
typedef int FestringCoord;
#else
typedef uint16_t FestringCoord;
#endif

void festring_setcursor(FestringCoord x, FestringCoord y);
void festring_setbound(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);
void festring_settextcolor(uint16_t color);
void festring_setbackcolor(uint16_t color);
void festring_setdropcolor(uint16_t color);
void festring_setlinewrap(int16_t enable);
void festring_setautofill(int16_t enable);
void festring_setfontsize(int size);
void festring_farstrcpy(const char* src);
void festring_farstrcat(const char* src);
void festring_farstradd(char c);
void festring_outstring(const uint8_t* s);
void festring_outstringcenter(const uint8_t* s);
void festring_outstringright(const uint8_t* s);
void festring_clearscreen(void);
void festring_hidescreen(void);
void festring_showscreen(void);

#ifdef TIE_MODERN
/* One captured run of text: where it started and its characters, with
 * 0xFE+colour escapes where the (remapped) text colour changes. */
typedef struct FestringCaptureLine {
	int16_t x, y;
	uint8_t color;
	char text[64];
} FestringCaptureLine;

/* Text capture: until festring_captureend, characters sent through outchar
 * are recorded into `lines` instead of drawn. festring state (cursor,
 * colours, margins, wrap, font) is restored by festring_captureend, which
 * returns the number of lines recorded. */
void festring_capturebegin(FestringCaptureLine* lines, int capacity);
int festring_captureend(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
