// cbvga_palette_emulator.c
// Bootstrapper for CSMWrap: provides globals and initializes palette state.
// Leaves actual emulation logic to the vgasrc implementation.

#include "cbvga_palette_emulator.h"
#include "rep-string.h"
#include "rep-stdio.h"   // use printf from libc/nanoprintf

// -----------------------------------------------------------------------------
// Globals expected by vgasrc
// -----------------------------------------------------------------------------
u8  cbvga_palette[256][3];
u32 cbvga_palette_argb[256];

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------
static inline u32 argb(u8 a, u8 r, u8 g, u8 b) {
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

// VGA DAC values are 6-bit (0–63). Expand to 8-bit (0–255).
// Optionally swap R/B channels if your framebuffer expects BGRA.
static void rebuild_argb_table(void)
{
    for (int i = 0; i < 256; i++) {
        // Scale up 6-bit DAC values
        u8 r8 = cbvga_palette[i][0] << 2;
        u8 g8 = cbvga_palette[i][1] << 2;
        u8 b8 = cbvga_palette[i][2] << 2;

#ifdef CBVGA_USE_BGRA
        // BGRA packing (common on Intel GOP)
        cbvga_palette_argb[i] = (0xFFu << 24) | (b8 << 16) | (g8 << 8) | r8;
#else
        // ARGB packing
        cbvga_palette_argb[i] = (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8;
#endif
    }
}

// Default 16‑color VGA palette (6‑bit values)
static const u8 default_vga16_6bit[16][3] = {
    { 0,  0,  0},  { 0,  0, 42},  { 0, 42,  0},  { 0, 42, 42},
    {42,  0,  0},  {42,  0, 42},  {42, 21,  0},  {42, 42, 42},
    {21, 21, 21},  {21, 21, 63},  {21, 63, 21},  {21, 63, 63},
    {63, 21, 21},  {63, 21, 63},  {63, 63, 21},  {63, 63, 63},
};

// -----------------------------------------------------------------------------
// Public bootstrap API
// -----------------------------------------------------------------------------
void cbvga_palette_set_entries(const void *entries, int count) {
    if (!entries || count <= 0) {
        // Load default 16‑color palette
        memset(cbvga_palette, 0, sizeof(cbvga_palette));
        for (int i = 0; i < 16; i++)
            memcpy(cbvga_palette[i], default_vga16_6bit[i], 3);
    } else {
        if (count > 256) count = 256;
        const u8 *p = (const u8 *)entries;
        for (int i = 0; i < count; i++) {
            cbvga_palette[i][0] = p[i*3 + 0];
            cbvga_palette[i][1] = p[i*3 + 1];
            cbvga_palette[i][2] = p[i*3 + 2];
        }
    }
    rebuild_argb_table();
}

// -----------------------------------------------------------------------------
// Diagnostic dump (called by csmwrap.c)
// -----------------------------------------------------------------------------
void cbvga_emul_dump_state(void) {
    printf("CSMwrap: VGA palette state (first 16 entries):\n");
    for (int i = 0; i < 16; i++) {
        u8 r = cbvga_palette[i][0];
        u8 g = cbvga_palette[i][1];
        u8 b = cbvga_palette[i][2];
        u32 argb = cbvga_palette_argb[i];
        printf("  Idx %02d: DAC=(%3u,%3u,%3u) ARGB=0x%08X\n",
               i, r, g, b, argb);
    }
}