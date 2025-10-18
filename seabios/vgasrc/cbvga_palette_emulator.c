// cbvga_palette_emulator.c
// Functional palette emulator for CSMWrap/SeaBIOS VGA
// Licensed under GNU LGPLv3

#include <string.h>   // for memcpy
#include <stdint.h>   // for uint8_t etc. if not already in types.h

#include "types.h"
#include "vgabios.h"
#include "util.h"
#include "farptr.h"
#include "config.h"
#include "output.h"
#include "std/vbe.h"
#include "cbvga_palette_emulator.h"

// -----------------------------------------------------------------------------
// Raw VGA DAC palette (256 x RGB), and ARGB lookup built from it
// -----------------------------------------------------------------------------
u8  cbvga_palette[256][3] __attribute__((section(".data16.cbvga")));
u32 cbvga_palette_argb[256] __attribute__((section(".data16.cbvga")));

// Default 16‑color VGA palette (6‑bit DAC values, 0–63 range)
static const u8 default_vga_palette[16][3] = {
    {  0,  0,  0 },   // 0 black
    {  0,  0, 42 },   // 1 blue
    {  0, 42,  0 },   // 2 green
    {  0, 42, 42 },   // 3 cyan
    { 42,  0,  0 },   // 4 red
    { 42,  0, 42 },   // 5 magenta
    { 42, 21,  0 },   // 6 brown
    { 42, 42, 42 },   // 7 light gray
    { 21, 21, 21 },   // 8 dark gray
    { 21, 21, 63 },   // 9 bright blue
    { 21, 63, 21 },   // 10 bright green
    { 21, 63, 63 },   // 11 bright cyan
    { 63, 21, 21 },   // 12 bright red
    { 63, 21, 63 },   // 13 bright magenta
    { 63, 63, 21 },   // 14 yellow
    { 63, 63, 63 }    // 15 white
};

// 256‑color VGA palette (6‑bit DAC values, 0–63 range)
static u8 default_vga256_palette[256][3];

static void init_vga256_palette(void) {
    // First 16 entries: classic VGA palette
    for (int i = 0; i < 16; i++) {
        default_vga256_palette[i][0] = default_vga_palette[i][0];
        default_vga256_palette[i][1] = default_vga_palette[i][1];
        default_vga256_palette[i][2] = default_vga_palette[i][2];
    }

    // 6×6×6 color cube (216 colors, indices 16–231)
    int idx = 16;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                default_vga256_palette[idx][0] = r * 63 / 5; // red
                default_vga256_palette[idx][1] = g * 63 / 5; // green
                default_vga256_palette[idx][2] = b * 63 / 5; // blue
                idx++;
            }
        }
    }

    // Grayscale ramp (24 shades, indices 232–255)
    for (int i = 0; i < 24; i++, idx++) {
        u8 val = (u8)((i * 63) / 23);
        default_vga256_palette[idx][0] = val;
        default_vga256_palette[idx][1] = val;
        default_vga256_palette[idx][2] = val;
    }
}

static inline u8 nearest_vga16_index(u32 c) {
    u8 r = (u8)(c);
    u8 g = (u8)(c >> 8);
    u8 b = (u8)(c >> 16);

    int best = 0;
    int bestdist = 999999;

    for (int i = 0; i < 16; i++) {
        int pr = default_vga_palette[i][0] << 2;
        int pg = default_vga_palette[i][1] << 2;
        int pb = default_vga_palette[i][2] << 2;

        int dr = (int)r - pr;
        int dg = (int)g - pg;
        int db = (int)b - pb;
        int dist = dr*dr + dg*dg + db*db;

        if (dist < bestdist) {
            bestdist = dist;
            best = i;
        }
    }
    return (u8)best;
}

static inline u8 nearest_vga256_index(u32 c) {
    u8 r = (u8)(c);
    u8 g = (u8)(c >> 8);
    u8 b = (u8)(c >> 16);

    int best = 0;
    int bestdist = 999999;

    for (int i = 0; i < 256; i++) {
        int pr = default_vga256_palette[i][0] << 2; // expand 6‑bit to 8‑bit
        int pg = default_vga256_palette[i][1] << 2;
        int pb = default_vga256_palette[i][2] << 2;

        int dr = (int)r - pr;
        int dg = (int)g - pg;
        int db = (int)b - pb;
        int dist = dr*dr + dg*dg + db*db;

        if (dist < bestdist) {
            bestdist = dist;
            best = i;
        }
    }
    return (u8)best;
}

// -----------------------------------------------------------------------------
// Emulator state (placed in 16‑bit data segment so linker keeps them)
// -----------------------------------------------------------------------------
static const void *g_shadow_ptr __attribute__((section(".data16.cbvga"))) = NULL;
static void *g_dst_ptr          __attribute__((section(".data16.cbvga"))) = NULL;

int g_src_pitch  VAR16;
int g_dst_pitch  VAR16;

int g_width      VAR16;
int g_height     VAR16;
int g_src_bpp    VAR16;
int g_dst_bpp    VAR16;

u8 g_red_mask_size   VAR16;
u8 g_green_mask_size VAR16;
u8 g_blue_mask_size  VAR16;

u8 g_red_mask_pos    VAR16;
u8 g_green_mask_pos  VAR16;
u8 g_blue_mask_pos   VAR16;

// forward declaration so compiler knows about it
u16 pack_rgb16(u8 r, u8 g, u8 b,
               u8 rsize, u8 gsize, u8 bsize,
               u8 rpos,  u8 gpos,  u8 bpos);

// Forward declaration so compiler knows about packed_to_argb
static inline u32 packed_to_argb(u32 pixel,
                                 u8 rpos, u8 rsize,
                                 u8 gpos, u8 gsize,
                                 u8 bpos, u8 bsize);

// Expand a source pixel at x from sline to ARGB32
static inline u32 expand_src_pixel(const u8 *sline, int x, int src_bpp) {
    if (src_bpp == 4) {
        u8 byte = sline[x >> 1];
        u8 idx  = (x & 1) ? (byte & 0x0F) : (byte >> 4);
        return cbvga_palette_argb[idx];
    } else if (src_bpp == 8) {
        return cbvga_palette_argb[sline[x]];
    } else if (src_bpp == 16) {
        const u16 *s16 = (const u16*)sline;
        return packed_to_argb((u32)s16[x],
                              g_red_mask_pos,   g_red_mask_size,
                              g_green_mask_pos, g_green_mask_size,
                              g_blue_mask_pos,  g_blue_mask_size);
    } else if (src_bpp == 24) {
        u32 p24 = ((u32)sline[x*3+0]) |
                  ((u32)sline[x*3+1] << 8) |
                  ((u32)sline[x*3+2] << 16);
        return packed_to_argb(p24,
                              g_red_mask_pos,   g_red_mask_size,
                              g_green_mask_pos, g_green_mask_size,
                              g_blue_mask_pos,  g_blue_mask_size);
    } else { // 32-bit
        const u32 *s32 = (const u32*)sline;
        return s32[x];
    }
    return 0; // fallback
}

// -----------------------------------------------------------------------------
// Rebuild ARGB table from current cbvga_palette (like CSM does)
// -----------------------------------------------------------------------------
static void rebuild_argb_table(void)
{
    for (int i = 0; i < 256; i++) {
        u8 r = cbvga_palette[i][0] << 2;
        u8 g = cbvga_palette[i][1] << 2;
        u8 b = cbvga_palette[i][2] << 2;

        // Pack as BGRA (Intel GOP convention)
        cbvga_palette_argb[i] = 0xFF000000u
                              | ((u32)b << 16)
                              | ((u32)g << 8)
                              | (u32)r;
    }
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline u8 expand_chan(u32 pixel, u8 pos, u8 size) {
    if (!size) return 0;
    u32 mask = (1u << size) - 1u;
    u32 val  = (pixel >> pos) & mask;
    return (u8)((val * 255u) / mask);
}

static inline u32 packed_to_argb(u32 pixel,
                                 u8 rpos, u8 rsize,
                                 u8 gpos, u8 gsize,
                                 u8 bpos, u8 bsize) {
    u8 r = expand_chan(pixel, rpos, rsize);
    u8 g = expand_chan(pixel, gpos, gsize);
    u8 b = expand_chan(pixel, bpos, bsize);
    return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

// -----------------------------------------------------------------------------
// Public setters
// -----------------------------------------------------------------------------
void cbvga_emul_set_shadow(const void *ptr, int pitch_bytes) {
    g_shadow_ptr = ptr;
    g_src_pitch  = pitch_bytes;
}

void cbvga_emul_set_masks(u8 r_size, u8 g_size, u8 b_size,
                          u8 r_pos,  u8 g_pos,  u8 b_pos) {
    g_red_mask_size   = r_size;
    g_green_mask_size = g_size;
    g_blue_mask_size  = b_size;
    g_red_mask_pos    = r_pos;
    g_green_mask_pos  = g_pos;
    g_blue_mask_pos   = b_pos;
}

// -----------------------------------------------------------------------------
// Palette programming
// -----------------------------------------------------------------------------
void cbvga_emul_set_palette(int start, int count, u8 *data) {
    for (int i = 0; i < count; i++) {
        int idx = start + i;
        if (idx >= 256) break;

        // Store raw 6‑bit DAC values (0–63 range)
        cbvga_palette[idx][0] = data[i*3 + 0];
        cbvga_palette[idx][1] = data[i*3 + 1];
        cbvga_palette[idx][2] = data[i*3 + 2];
    }

    // Rebuild ARGB lookup table once after all updates
    rebuild_argb_table();
}

// -----------------------------------------------------------------------------
// Simple palette helper for text‑mode emulation
// -----------------------------------------------------------------------------
void cbvga_palette_set_entries(const void *entries, int count) {
    if (!entries) {
        // Load default 16‑color VGA palette
        for (int i = 0; i < 16; i++) {
            cbvga_palette[i][0] = default_vga_palette[i][0];
            cbvga_palette[i][1] = default_vga_palette[i][1];
            cbvga_palette[i][2] = default_vga_palette[i][2];
        }
    } else {
        const u8 *src = (const u8*)entries;
        for (int i = 0; i < count && i < 256; i++) {
            cbvga_palette[i][0] = src[i*3 + 0];
            cbvga_palette[i][1] = src[i*3 + 1];
            cbvga_palette[i][2] = src[i*3 + 2];
        }
    }

    // Always rebuild ARGB lookup after updating DAC
    rebuild_argb_table();
}

// -----------------------------------------------------------------------------
// Debug pattern selector
// 0 = none, 1 = bars, 2 = checkerboard, 3 = combined
// -----------------------------------------------------------------------------
#define CBVGA_DEBUG 0

// Forward declarations of debug helpers
void cbvga_test_pattern(void);
void cbvga_test_checkerboard(void);
void cbvga_test_combined(void);

// -----------------------------------------------------------------------------
// Mode setup
// -----------------------------------------------------------------------------
int cbvga_emul_set_mode(struct vgamode_s *vmode_g, int flags) {
    g_shadow_ptr = NULL;

    g_width      = GET_FARVAR(0, vmode_g->width);
    g_height     = GET_FARVAR(0, vmode_g->height);
    g_src_bpp    = GET_FARVAR(0, vmode_g->depth);
    g_src_pitch  = 0;

    // Initialize grayscale palette if requested
    if (flags & MF_GRAYSUM) {
        for (int i = 0; i < 256; i++) {
            u8 dacval = i >> 2; // 0–255 → 0–63
            cbvga_palette[i][0] = dacval;
            cbvga_palette[i][1] = dacval;
            cbvga_palette[i][2] = dacval;
        }
        rebuild_argb_table();
    }

    // --- Universal mask setup from VBE mode info ---
    struct vbe_mode_info *info = get_current_vbe_mode_info();
    if (info) {
        // Set destination bpp from mode info
        g_dst_bpp = info->bits_per_pixel;

        // Clamp to supported values (4/8/16/24/32)
        if (g_dst_bpp != 4 && g_dst_bpp != 8 &&
            g_dst_bpp != 16 && g_dst_bpp != 24 &&
            g_dst_bpp != 32) {
            g_dst_bpp = 32; // safe fallback
        }

        cbvga_emul_set_masks(info->red_size, info->green_size, info->blue_size,
                             info->red_pos,  info->green_pos,  info->blue_pos);

        // Use linear pitch if available, else fallback
        g_dst_pitch = info->linear_bytes_per_scanline
                        ? info->linear_bytes_per_scanline
                        : info->bytes_per_scanline;

        if (g_src_bpp == 4) {
            // Packed 4bpp stride: ceil(width/2), aligned to 4 bytes
            g_src_pitch = (((g_width + 1) / 2) + 3) & ~3;
        } else {
            g_src_pitch = g_dst_pitch;
        }

        // --- NEW: initialize default VGA256 palette if in 8-bit mode ---
        if (g_dst_bpp == 8) {
            init_vga256_palette();
        }
    }

#if CBVGA_DEBUG == 1
    cbvga_test_pattern();
#elif CBVGA_DEBUG == 2
    cbvga_test_checkerboard();
#elif CBVGA_DEBUG == 3
    cbvga_test_combined();
#endif

    return 0;
}

void cbvga_emul_setup_modes(u64 addr, u8 bpp,
                            u32 xlines, u32 ylines, u32 linelength) {
    g_dst_ptr   = (void*)(u32)addr;
    g_width     = xlines;
    g_height    = ylines;
    g_src_bpp   = bpp;        // guest depth (4/8/16/24/32)
    g_dst_bpp   = 24;
    g_dst_pitch = linelength; // GOP-reported pitch
}

// -----------------------------------------------------------------------------
// DAC format helpers
// -----------------------------------------------------------------------------
int cbvga_emul_get_dacformat(struct vgamode_s *curmode_g) {
    return 0x0606; // legacy 6-bit DAC
}

int cbvga_emul_set_dacformat(struct vgamode_s *curmode_g, int val) {
    return 0;
}

// -----------------------------------------------------------------------------
// Flush: shadow → native framebuffer
// Expands guest 4/8/16/24/32‑bit sources into true‑color or indexed host buffer
// Supports 32‑bit (BGRA), 24‑bit (BGR), 16‑bit (RGB565), 8‑bit (indexed),
// and 4‑bit (indexed, packed nibbles: 2 pixels per byte).
// -----------------------------------------------------------------------------
void cbvga_pal_flush(void) {
    if (!g_shadow_ptr || !g_dst_ptr) return;

    const int bytespp_dst = (g_dst_bpp == 24) ? 3 :
                            (g_dst_bpp == 16) ? 2 :
                            (g_dst_bpp == 8)  ? 1 :
                            (g_dst_bpp == 4)  ? 0 /*special*/ : 4;

    for (int y = 0; y < g_height; y++) {
        const u8 *sline = (const u8*)g_shadow_ptr + y * g_src_pitch;
        u8  *dline8  = (u8*)g_dst_ptr + y * g_dst_pitch;
        u16 *dline16 = (u16*)dline8;
        u32 *dline32 = (u32*)dline8;

        int maxpix_src = (g_src_bpp == 4) ? (g_src_pitch * 2)
                                          : (g_src_pitch / (g_src_bpp / 8));
        int maxpix_dst = (g_dst_bpp == 4) ? (g_dst_pitch * 2)
                          : (g_dst_pitch / ((bytespp_dst == 0) ? 1 : bytespp_dst));

        int limit = g_width;
        if (limit > maxpix_dst) limit = maxpix_dst;
        if (limit > maxpix_src) limit = maxpix_src;

        // 4-bit destination: pack two indices per byte (high nibble = x, low = x+1)
        if (g_dst_bpp == 4) {
            u8 *dline4 = (u8*)g_dst_ptr + y * g_dst_pitch;

            if (g_src_bpp == 4) {
                int bytes_to_copy = (limit + 1) >> 1;
                memcpy(dline4, sline, bytes_to_copy);
            } else if (g_src_bpp == 8) {
                for (int x = 0; x < limit; x += 2) {
                    u8 idx0 = sline[x] & 0x0F;
                    u8 idx1 = (x + 1 < limit) ? (sline[x + 1] & 0x0F) : 0;
                    dline4[x >> 1] = (idx0 << 4) | idx1;
                }
            } else {
                for (int x = 0; x < limit; x += 2) {
                    u32 c0 = expand_src_pixel(sline, x, g_src_bpp);
                    u32 c1 = (x + 1 < limit) ? expand_src_pixel(sline, x + 1, g_src_bpp) : c0;
                    u8 idx0 = nearest_vga16_index(c0);
                    u8 idx1 = nearest_vga16_index(c1);
                    dline4[x >> 1] = (idx0 << 4) | idx1;
                }
            }
            continue;
        }

        // 8-bit destination (indexed)
        if (g_dst_bpp == 8) {
            if (g_src_bpp == 4) {
                for (int x = 0; x < limit; x += 2) {
                    u8 byte = sline[x >> 1];
                    u8 idx0 = (byte >> 4) & 0x0F;
                    u8 idx1 = byte & 0x0F;
                    dline8[x] = idx0;
                    if (x + 1 < limit) dline8[x + 1] = idx1;
                }
            } else if (g_src_bpp == 8) {
                memcpy(dline8, sline, limit);
            } else {
                for (int x = 0; x < limit; x++) {
                    u32 c = expand_src_pixel(sline, x, g_src_bpp);
                    dline8[x] = nearest_vga256_index(c);
                }
            }
            continue;
        }

        // True-color destinations: expand to ARGB then down-pack if needed
        for (int x = 0; x < limit; x++) {
            u32 c = expand_src_pixel(sline, x, g_src_bpp);

            if (g_dst_bpp == 32) {
                dline32[x] = c;
            } else if (g_dst_bpp == 24) {
                dline8[x*3+0] = (u8)(c      ); // B
                dline8[x*3+1] = (u8)(c >> 8); // G
                dline8[x*3+2] = (u8)(c >>16); // R
            } else if (g_dst_bpp == 16) {
                u8 r = (u8)(c      ), g = (u8)(c >> 8), b = (u8)(c >>16);
                u16 rgb565 = ((r >> 3) << 11) |
                             ((g >> 2) << 5)  |
                             ((b >> 3));
                dline16[x] = rgb565;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Debug test patterns
// -----------------------------------------------------------------------------
void cbvga_test_pattern(void) {
    if (!g_dst_ptr || !g_width || !g_height) return;

    static const u32 bars[] = {
        0xFF000000, // black
        0xFFFFFFFF, // white
        0xFFFF0000, // red
        0xFF00FF00, // green
        0xFF0000FF, // blue
        0xFF00FFFF, // cyan
        0xFFFF00FF, // magenta
        0xFFFFFF00, // yellow
        0xFF808080  // gray
    };
    const int n_bars = sizeof(bars)/sizeof(bars[0]);
    int bar_width = g_width / n_bars;

    for (int y = 0; y < g_height; y++) {
        u32 *dline = (u32*)((u8*)g_dst_ptr + y * g_dst_pitch);
        for (int x = 0; x < g_width; x++) {
            int idx = x / bar_width;
            if (idx >= n_bars) idx = n_bars - 1;
            dline[x] = bars[idx];
        }
    }
}

void cbvga_test_checkerboard(void) {
    if (!g_dst_ptr || !g_width || !g_height) return;

    const int square = 8;
    for (int y = 0; y < g_height; y++) {
        u32 *dline = (u32*)((u8*)g_dst_ptr + y * g_dst_pitch);
        int yblock = (y / square) & 1;
        for (int x = 0; x < g_width; x++) {
            int xblock = (x / square) & 1;
            u32 color = ((xblock ^ yblock) ? 0xFFFFFFFF : 0xFF000000);
            dline[x] = color;
        }
    }
}

void cbvga_test_combined(void) {
    if (!g_dst_ptr || !g_width || !g_height) return;

    static const u32 bars[] = {
        0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00,
        0xFF0000FF, 0xFF00FFFF, 0xFFFF00FF, 0xFFFFFF00, 0xFF808080
    };
    const int n_bars = sizeof(bars)/sizeof(bars[0]);
    int bar_width = g_width / n_bars;
    int half_height = g_height / 2;

    for (int y = 0; y < g_height; y++) {
        u32 *dline = (u32*)((u8*)g_dst_ptr + y * g_dst_pitch);
        if (y < half_height) {
            for (int x = 0; x < g_width; x++) {
                int idx = x / bar_width;
                if (idx >= n_bars) idx = n_bars - 1;
                dline[x] = bars[idx];
            }
        } else {
            int yblock = ((y - half_height) / 8) & 1;
            for (int x = 0; x < g_width; x++) {
                int xblock = (x / 8) & 1;
                u32 color = ((xblock ^ yblock) ? 0xFFFFFFFF : 0xFF000000);
                dline[x] = color;
            }
        }
    }
}

void cbvga_emul_dump_state(void) {
    dprintf(1, "Emul: dst_ptr=%p dst_bpp=%d src_bpp=%d pitch_dst=%d pitch_src=%d w=%d h=%d\n",
            g_dst_ptr, g_dst_bpp, g_src_bpp, g_dst_pitch, g_src_pitch, g_width, g_height);
    dprintf(1, "Emul: masks R(%u@%u) G(%u@%u) B(%u@%u)\n",
            g_red_mask_size, g_red_mask_pos,
            g_green_mask_size, g_green_mask_pos,
            g_blue_mask_size,  g_blue_mask_pos);
}
