// seabios/vgasrc/vgafonts.h
// Extern declaration for the 8x16 font table defined in vgafonts.c

#pragma once

#include <stdint.h>

// 256 glyphs, each 16 rows of 8 pixels
extern const uint8_t font_8x16[256][16];