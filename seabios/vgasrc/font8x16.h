// seabios/vgasrc/font8x16.h
// 8x16 bitmap font for text mode emulation.
// Each glyph is 16 bytes, one byte per row, MSB = leftmost pixel.
// This is a CP437-compatible font (subset shown here).

#pragma once
#include <stdint.h>

// 256 glyphs, each 16 rows of 8 pixels
extern const uint8_t font8x16[256][16];