// cbvga_palette_emulator.h
// Functional palette emulator interface for CSMWrap/SeaBIOS VGA
// Licensed under GNU LGPLv3

#ifndef __CBVGA_PALETTE_EMULATOR_H
#define __CBVGA_PALETTE_EMULATOR_H

#include "types.h"
#include "vgabios.h"

// Raw VGA DAC (256 x RGB)
extern u8  cbvga_palette[256][3];
extern u32 cbvga_palette_argb[256];

// DAC format helpers (6-bit legacy by default)
int cbvga_emul_get_dacformat(struct vgamode_s *curmode_g);
int cbvga_emul_set_dacformat(struct vgamode_s *curmode_g, int val);

// Palette programming (updates DAC and ARGB lookup)
void cbvga_emul_set_palette(int start, int count, u8 *data);

// Mode setup (captures grayscale/nopalette flags and geometry)
int  cbvga_emul_set_mode(struct vgamode_s *vmode_g, int flags);
void cbvga_emul_setup_modes(u64 addr, u8 bpp, u32 xlines, u32 ylines, u32 linelength);

// Provide/replace the shadow buffer pointer (4/8/16/24/32bpp supported)
void cbvga_emul_set_shadow(const void *ptr, int pitch_bytes);

// Flush: blit shadow → ARGB32 framebuffer using palette/conversions
void cbvga_pal_flush(void);

// Dump emulator runtime state to log (for CSMwrap pre-SeaBIOS checks)
void cbvga_emul_dump_state(void);

// Set channel mask sizes and positions for generalized decode
// Sizes distinguish 16bpp formats (e.g., 5-6-5 vs 5-5-5).
// Positions determine RGB vs BGR order and exact bit offsets (24/32bpp).
void cbvga_emul_set_masks(u8 r_size, u8 g_size, u8 b_size,
                          u8 r_pos,  u8 g_pos,  u8 b_pos);

// -----------------------------------------------------------------------------
// Debug test patterns (optional)
// -----------------------------------------------------------------------------
void cbvga_test_pattern(void);       // vertical color bars
void cbvga_test_checkerboard(void);  // checkerboard squares
void cbvga_test_combined(void);      // bars top half, checkerboard bottom half

// -----------------------------------------------------------------------------
// Simple palette helpers for text‑mode emulation
// -----------------------------------------------------------------------------

// Return ARGB value for a given VGA color index (0–255).
static inline u32 cbvga_palette_lookup(int idx) {
    return cbvga_palette_argb[idx & 0xFF];
}

// Reset or set the first N palette entries.
// If 'entries' is NULL, load the default 16‑color VGA palette.
void cbvga_palette_set_entries(const void *entries, int count);

#endif // __CBVGA_PALETTE_EMULATOR_H