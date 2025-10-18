// cbvga.c
// Simple framebuffer vgabios for use with coreboot native vga init.
// Licensed under GNU LGPLv3

#include "biosvar.h"   // GET_BDA
#include "output.h"    // dprintf
#include "stdvga.h"    // SEG_CTEXT
#include "string.h"    // memset16_far
#include "util.h"      // find_cb_table
#include "vgabios.h"   // SET_VGA
#include "vgafb.h"     // handle_gfx_op
#include "vgautil.h"   // VBE_total_memory
#include "svgamodes.h" // svga_modes
#include "cbvga_palette_emulator.h" // palette emulator integration
#include <stdint.h>
#include "cbvga_shared.h"

static int CBmode VAR16;
static struct vgamode_s CBmodeinfo VAR16;
static struct vgamode_s CBemulinfo VAR16;
static u32 CBlinelength VAR16;
u8 CBred_size   VAR16;
u8 CBgreen_size VAR16;
u8 CBblue_size  VAR16;
u8 CBred_pos    VAR16;
u8 CBgreen_pos  VAR16;
u8 CBblue_pos   VAR16;


void cbvga_init_text_palette(void);

// -----------------------------------------------------------------------------
// Mode lookup and listing
// -----------------------------------------------------------------------------
struct vgamode_s *cbvga_find_mode(int mode)
{
    if (mode == GET_GLOBAL(CBmode))
        return &CBmodeinfo;
    if (mode == 0x03)
        return &CBemulinfo;

    for (int i = 0; i < GET_GLOBAL(svga_mcount); i++) {
        struct generic_svga_mode *cbmode_g = &svga_modes[i];
        if (GET_GLOBAL(cbmode_g->mode) == 0xffff)
            continue;
        if (GET_GLOBAL(cbmode_g->mode) == mode)
            return &cbmode_g->info;
    }
    return NULL;
}

void cbvga_list_modes(u16 seg, u16 *dest, u16 *last)
{
    int seen = 0;

    if (GET_GLOBAL(CBmode) != 0x3) {
        for (int i = 0; i < GET_GLOBAL(svga_mcount) && dest < last; i++) {
            struct generic_svga_mode *cbmode_g = &svga_modes[i];
            u16 mode = GET_GLOBAL(cbmode_g->mode);
            if (mode == 0xffff)
                continue;
            SET_FARVAR(seg, *dest, mode);
            dest++;
            if (GET_GLOBAL(CBmode) == mode)
                seen = 1;
        }
    }
    if (dest < last && !seen) {
        SET_FARVAR(seg, *dest, GET_GLOBAL(CBmode));
        dest++;
    }
    SET_FARVAR(seg, *dest, 0xffff);
}

// -----------------------------------------------------------------------------
// Banked window (unsupported)
// -----------------------------------------------------------------------------
int cbvga_get_window(struct vgamode_s *curmode_g, int window) { return -1; }
int cbvga_set_window(struct vgamode_s *curmode_g, int window, int val) { return -1; }

// -----------------------------------------------------------------------------
// Pitch and display start
// -----------------------------------------------------------------------------
int cbvga_minimum_linelength(struct vgamode_s *vmode_g)
{
    return GET_GLOBAL(CBlinelength);
}
int cbvga_get_linelength(struct vgamode_s *curmode_g)
{
    return GET_GLOBAL(CBlinelength);
}
int cbvga_set_linelength(struct vgamode_s *curmode_g, int val) { return -1; }
int cbvga_get_displaystart(struct vgamode_s *curmode_g) { return 0; }
int cbvga_set_displaystart(struct vgamode_s *curmode_g, int val) { return -1; }

// -----------------------------------------------------------------------------
// DAC format (delegated to palette emulator)
// -----------------------------------------------------------------------------
int cbvga_get_dacformat(struct vgamode_s *curmode_g)
{
    return cbvga_emul_get_dacformat(curmode_g);
}
int cbvga_set_dacformat(struct vgamode_s *curmode_g, int val)
{
    return cbvga_emul_set_dacformat(curmode_g, val);
}

// -----------------------------------------------------------------------------
// Save/restore and set_mode
// -----------------------------------------------------------------------------
int cbvga_save_restore(int cmd, u16 seg, void *data)
{
    if (cmd & (SR_HARDWARE|SR_DAC|SR_REGISTERS))
        return -1;
    return bda_save_restore(cmd, seg, data);
}

int cbvga_set_mode(struct vgamode_s *vmode_g, int flags)
{
    u8 emul = vmode_g == &CBemulinfo || GET_GLOBAL(CBmode) == 0x03;

    u8 extra_stack = GET_BDA_EXT(flags) & BF_EXTRA_STACK;
    MASK_BDA_EXT(flags, BF_EMULATE_TEXT, emul ? BF_EMULATE_TEXT : 0);

    if (!(flags & MF_NOCLEARMEM)) {
        if (GET_GLOBAL(CBmodeinfo.memmodel) == MM_TEXT) {
            memset16_far(SEG_CTEXT, (void*)0, 0x0720, 80*25*2);
            cbvga_emul_set_mode(&CBmodeinfo, flags);
            return 0;
        }
        if (extra_stack || flags & MF_LEGACY) {
            u32 pitch = GET_GLOBAL(CBlinelength);
            u32 w = GET_GLOBAL(CBmodeinfo.width);
            u32 bpp = GET_GLOBAL(CBmodeinfo.depth);
            u32 min_stride = (bpp == 32 ? w*4 :
                              bpp == 24 ? w*3 :
                              bpp == 16 ? w*2 : 0);
            if (pitch >= min_stride && GET_GLOBAL(VBE_framebuffer)) {
                struct gfx_op op;
                init_gfx_op(&op, &CBmodeinfo);
                op.x = op.y = 0;
                op.xlen = w;
                op.ylen = GET_GLOBAL(CBmodeinfo.height);
                op.op = GO_MEMSET;
                handle_gfx_op(&op);
            }
        }
    }

    cbvga_emul_setup_modes(
        GET_GLOBAL(VBE_framebuffer),
        GET_GLOBAL(vmode_g->depth),
        GET_GLOBAL(vmode_g->width),
        GET_GLOBAL(vmode_g->height),
        GET_GLOBAL(CBlinelength)
    );

    cbvga_emul_set_mode(&CBmodeinfo, flags);
    return 0;
}

// -----------------------------------------------------------------------------
// Coreboot framebuffer tag
// -----------------------------------------------------------------------------
#define CB_TAG_FRAMEBUFFER      0x0012
struct cb_framebuffer {
    u32 tag;
    u32 size;
    u64 physical_address;
    u32 x_resolution;
    u32 y_resolution;
    u32 bytes_per_line;
    u8 bits_per_pixel;
    u8 red_mask_pos;
    u8 red_mask_size;
    u8 green_mask_pos;
    u8 green_mask_size;
    u8 blue_mask_pos;
    u8 blue_mask_size;
    u8 reserved_mask_pos;
    u8 reserved_mask_size;
};

// -----------------------------------------------------------------------------
// Helpers to synthesize 4/8/16/24/32‑bpp modes for each resolution
// -----------------------------------------------------------------------------
#define SVGA_MAX_MODES 128

static void add_mode_depths_for_resolution(u16 base_mode,
                                           u32 x, u32 y,
                                           u64 addr, u32 pitch)
{
    // We now add 5 entries (4bpp, 8bpp, 16bpp, 24bpp, 32bpp)
    if (GET_GLOBAL(svga_mcount) + 5 > SVGA_MAX_MODES)
        return;

    struct generic_svga_mode *m;
    int idx;

    // 4-bit (16 colors) indexed
    idx = GET_GLOBAL(svga_mcount);
    m = &svga_modes[idx];
    SET_VGA(svga_mcount, idx + 1);
    SET_VGA(m->mode, base_mode - 1);       // unique ID for 4bpp
    SET_VGA(m->info.width, x);
    SET_VGA(m->info.height, y);
    SET_VGA(m->info.depth, 4);
    SET_VGA(m->info.memmodel, MM_PACKED);  // emulated packed-indexed
    SET_VGA(m->info.cwidth, 8);
    SET_VGA(m->info.cheight, 16);

    // 8-bit indexed
    idx = GET_GLOBAL(svga_mcount);
    m = &svga_modes[idx];
    SET_VGA(svga_mcount, idx + 1);
    SET_VGA(m->mode, base_mode);
    SET_VGA(m->info.width, x);
    SET_VGA(m->info.height, y);
    SET_VGA(m->info.depth, 8);
    SET_VGA(m->info.memmodel, MM_PACKED);

    // 16-bit RGB
    idx = GET_GLOBAL(svga_mcount);
    m = &svga_modes[idx];
    SET_VGA(svga_mcount, idx + 1);
    SET_VGA(m->mode, base_mode + 1);
    SET_VGA(m->info.width, x);
    SET_VGA(m->info.height, y);
    SET_VGA(m->info.depth, 16);
    SET_VGA(m->info.memmodel, MM_DIRECT);

    // 24-bit RGB
    idx = GET_GLOBAL(svga_mcount);
    m = &svga_modes[idx];
    SET_VGA(svga_mcount, idx + 1);
    SET_VGA(m->mode, base_mode + 2);
    SET_VGA(m->info.width, x);
    SET_VGA(m->info.height, y);
    SET_VGA(m->info.depth, 24);
    SET_VGA(m->info.memmodel, MM_DIRECT);

    // 32-bit XRGB8888
    idx = GET_GLOBAL(svga_mcount);
    m = &svga_modes[idx];
    SET_VGA(svga_mcount, idx + 1);
    SET_VGA(m->mode, base_mode + 3);
    SET_VGA(m->info.width, x);
    SET_VGA(m->info.height, y);
    SET_VGA(m->info.depth, 32);
    SET_VGA(m->info.memmodel, MM_DIRECT);
}

static void register_emulated_modes(u64 addr, u32 fb_x, u32 fb_y, u32 pitch)
{
    add_mode_depths_for_resolution(0x100, 640, 480, addr, pitch);
    add_mode_depths_for_resolution(0x110, 800, 600, addr, pitch);
    add_mode_depths_for_resolution(0x120, 1024, 768, addr, pitch);
    add_mode_depths_for_resolution(0x130, 1280, 1024, addr, pitch);
    add_mode_depths_for_resolution(0x140, fb_x, fb_y, addr, pitch);
}
// -----------------------------------------------------------------------------
// Mode setup
// -----------------------------------------------------------------------------
void cbvga_setup_modes(u64 addr, u8 bpp, u32 xlines, u32 ylines, u32 linelength)
{
    SET_VGA(svga_mcount, 0);
    SET_VGA(CBmode, 0x143);

    SET_VGA(VBE_framebuffer, addr);
    SET_VGA(VBE_total_memory, linelength * ylines);
    SET_VGA(CBlinelength, linelength);
    SET_VGA(CBmodeinfo.memmodel, MM_DIRECT);
    SET_VGA(CBmodeinfo.width, xlines);
    SET_VGA(CBmodeinfo.height, ylines);
    SET_VGA(CBmodeinfo.depth, bpp);
    SET_VGA(CBmodeinfo.cwidth, 8);
    SET_VGA(CBmodeinfo.cheight, 16);
    memcpy_far(get_global_seg(), &CBemulinfo,
               get_global_seg(), &CBmodeinfo, sizeof(CBemulinfo));

    register_emulated_modes(addr, xlines, ylines, linelength);
    cbvga_emul_setup_modes(addr, bpp, xlines, ylines, linelength);
}

// -----------------------------------------------------------------------------
// Setup entry point
// -----------------------------------------------------------------------------
int cbvga_setup(void)
{
    dprintf(1, "coreboot vga init\n");

    if (GET_GLOBAL(HaveRunInit))
        return 0;

    struct cb_header *cbh = find_cb_table();
    if (!cbh) {
        dprintf(1, "Unable to find coreboot table\n");
        return -1;
    }
    struct cb_framebuffer *cbfb = find_cb_subtable(cbh, CB_TAG_FRAMEBUFFER);
    if (!cbfb) {
        dprintf(1, "Did not find coreboot framebuffer - assuming EGA text\n");
        SET_VGA(CBmode, 0x03);
        SET_VGA(CBlinelength, 80*2);
        SET_VGA(CBmodeinfo.memmodel, MM_TEXT);
        SET_VGA(CBmodeinfo.width, 80);
        SET_VGA(CBmodeinfo.height, 25);
        SET_VGA(CBmodeinfo.depth, 4);
        SET_VGA(CBmodeinfo.cwidth, 9);
        SET_VGA(CBmodeinfo.cheight, 16);
        SET_VGA(CBmodeinfo.sstart, SEG_CTEXT);
        return 0;
    }

    u64 addr    = GET_FARVAR(0, cbfb->physical_address);
    u8 tag_bpp  = GET_FARVAR(0, cbfb->bits_per_pixel);
    u32 xlines  = GET_FARVAR(0, cbfb->x_resolution);
    u32 ylines  = GET_FARVAR(0, cbfb->y_resolution);
    u32 stride  = GET_FARVAR(0, cbfb->bytes_per_line);

    // Capture mask sizes and positions for emulator and renderers
    CBred_size   = GET_FARVAR(0, cbfb->red_mask_size);
    CBgreen_size = GET_FARVAR(0, cbfb->green_mask_size);
    CBblue_size  = GET_FARVAR(0, cbfb->blue_mask_size);
    CBred_pos    = GET_FARVAR(0, cbfb->red_mask_pos);
    CBgreen_pos  = GET_FARVAR(0, cbfb->green_mask_pos);
    CBblue_pos   = GET_FARVAR(0, cbfb->blue_mask_pos);

    // Derive effective bpp
    u8 eff_bpp = tag_bpp;
    if (eff_bpp != 16 && eff_bpp != 24 && eff_bpp != 32 && xlines) {
        u32 bytes_per_px = stride / xlines;
        if (bytes_per_px == 4) eff_bpp = 32;
        else if (bytes_per_px == 3) eff_bpp = 24;
        else if (bytes_per_px == 2) eff_bpp = 16;
    }

    // Don’t silently promote 24→32 unless you’re sure
    if (tag_bpp == 24 && (stride / xlines) == 4) {
        dprintf(1, "INFO: 24bpp with 4-byte stride (padding). Keeping 24bpp.\n");
        eff_bpp = 24;
    }

    // Minimum stride based on effective bpp
    u32 min_stride = (eff_bpp == 32 ? xlines * 4 :
                      eff_bpp == 24 ? xlines * 3 :
                      eff_bpp == 16 ? xlines * 2 : 0);

    dprintf(1, "Found FB @ %llx %ux%u tag_bpp=%u eff_bpp=%u stride=%u "
               "masks R%u:%u G%u:%u B%u:%u\n",
            addr, xlines, ylines, tag_bpp, eff_bpp, stride,
            CBred_pos, CBred_size,
            CBgreen_pos, CBgreen_size,
            CBblue_pos, CBblue_size);

    // Sanity check masks, fallback to defaults if invalid
    if (eff_bpp == 16 && (!CBred_size || !CBgreen_size || !CBblue_size)) {
        dprintf(1, "WARN: invalid 16bpp masks, forcing RGB565\n");
        CBred_pos = 11; CBred_size = 5;
        CBgreen_pos = 5; CBgreen_size = 6;
        CBblue_pos = 0; CBblue_size = 5;
    } else if ((eff_bpp == 24 || eff_bpp == 32) &&
               (!CBred_size || !CBgreen_size || !CBblue_size)) {
        dprintf(1, "WARN: invalid 24/32bpp masks, forcing XRGB8888\n");
        CBred_pos = 16; CBred_size = 8;
        CBgreen_pos = 8; CBgreen_size = 8;
        CBblue_pos = 0; CBblue_size = 8;
    }

    // Pass to palette emulator
    cbvga_emul_set_masks(CBred_size, CBgreen_size, CBblue_size,
                         CBred_pos,  CBgreen_pos,  CBblue_pos);

    if (!addr || addr > 0xffffffff || !stride || !xlines || !ylines ||
        (eff_bpp != 16 && eff_bpp != 24 && eff_bpp != 32) ||
        (min_stride && stride < min_stride)) {
        dprintf(1, "FB invalid, falling back to text mode\n");
        SET_VGA(CBmode, 0x03);
        SET_VGA(CBlinelength, 80*2);
        SET_VGA(CBmodeinfo.memmodel, MM_TEXT);
        SET_VGA(CBmodeinfo.width, 80);
        SET_VGA(CBmodeinfo.height, 25);
        SET_VGA(CBmodeinfo.depth, 4);
        SET_VGA(CBmodeinfo.cwidth, 9);
        SET_VGA(CBmodeinfo.cheight, 16);
        SET_VGA(CBmodeinfo.sstart, SEG_CTEXT);
        return 0;
    }

    cbvga_setup_modes(addr, eff_bpp, xlines, ylines, stride);

    // Seed VGA 16-color palette for text rendering
    cbvga_init_text_palette();

    return 0;
}

// -----------------------------------------------------------------------------
// Extra helpers for text-mode emulation into ARGB framebuffer
// -----------------------------------------------------------------------------

// Default VGA 16-color palette (ARGB32)
static const u32 vga16_argb[16] = {
    0xFF000000, // 0 black
    0xFF800000, // 1 dark red
    0xFF008000, // 2 dark green
    0xFF808000, // 3 dark yellow
    0xFF000080, // 4 dark blue
    0xFF800080, // 5 dark magenta
    0xFF008080, // 6 dark cyan
    0xFFC0C0C0, // 7 light gray
    0xFF808080, // 8 dark gray
    0xFFFF0000, // 9 bright red
    0xFF00FF00, // 10 bright green
    0xFFFFFF00, // 11 bright yellow
    0xFF0000FF, // 12 bright blue
    0xFFFF00FF, // 13 bright magenta
    0xFF00FFFF, // 14 bright cyan
    0xFFFFFFFF  // 15 white
};

// Initialize the VGA 16-color palette
void cbvga_init_text_palette(void) {
    for (int i = 0; i < 16; i++)
        cbvga_palette_argb[i] = vga16_argb[i];
}

// -----------------------------------------------------------------------------
// Pixel packing helpers
// -----------------------------------------------------------------------------

// Convert 8-bit per channel RGB into native 16-bit pixel
// using mask sizes/positions from cb_framebuffer
static inline u16 pack_rgb16(u8 r, u8 g, u8 b,
                             u8 rsize, u8 gsize, u8 bsize,
                             u8 rpos,  u8 gpos,  u8 bpos)
{
    u16 rr = (r >> (8 - rsize)) & ((1 << rsize) - 1);
    u16 gg = (g >> (8 - gsize)) & ((1 << gsize) - 1);
    u16 bb = (b >> (8 - bsize)) & ((1 << bsize) - 1);

    return (rr << rpos) | (gg << gpos) | (bb << bpos);
}

// Render one character cell at (cx, cy) into framebuffer (16/32-bit aware)
void cbvga_render_char(u16 cx, u16 cy, u8 ch, u8 attr) {
    int char_height = GET_BDA(char_height);
    struct segoff_s font = get_font_data(ch);

    u8 fg = attr & 0x0F;
    u8 bg = (attr >> 4) & 0x0F;
    int depth = GET_GLOBAL(CBmodeinfo.depth);

    for (int row = 0; row < char_height; row++) {
        u8 fontline = GET_FARVAR(font.seg, *(u8*)(font.offset + row));
        u8 *base = (u8*)(uintptr_t)GET_GLOBAL(VBE_framebuffer)
                 + (cy*char_height + row) * GET_GLOBAL(CBlinelength)
                 + cx * CBmodeinfo.cwidth * (depth / 8);

        for (int col = 0; col < CBmodeinfo.cwidth; col++) {
            u32 argb = (fontline & (0x80 >> col))
                       ? cbvga_palette_argb[fg]
                       : cbvga_palette_argb[bg];

            if (depth == 32) {
                ((u32*)base)[col] = argb;
            } else if (depth == 24) {
                u8 r = (argb >> 16) & 0xFF;
                u8 g = (argb >> 8)  & 0xFF;
                u8 b = (argb)       & 0xFF;
                u8 *p = base + col * 3;
                p[0] = b; p[1] = g; p[2] = r;   // ✅ BGR order
            } else if (depth == 16) {
                u8 r = (argb >> 16) & 0xFF;
                u8 g = (argb >> 8)  & 0xFF;
                u8 b = (argb)       & 0xFF;
                u16 pixel = pack_rgb16(r, g, b,
                                       CBred_size, CBgreen_size, CBblue_size,
                                       CBred_pos,  CBgreen_pos,  CBblue_pos);
                ((u16*)base)[col] = pixel;
            }
        }
    }
}

// Clear a rectangular region in framebuffer with background color
void cbvga_clear_region(u16 x, u16 y, u16 cols, u16 rows, u8 attr) {
    int char_height = GET_BDA(char_height);
    u8 bg = (attr >> 4) & 0x0F;
    int depth = GET_GLOBAL(CBmodeinfo.depth);

    for (int row = 0; row < rows * char_height; row++) {
        u8 *base = (u8*)(uintptr_t)GET_GLOBAL(VBE_framebuffer)
                 + (y*char_height + row) * GET_GLOBAL(CBlinelength)
                 + x * CBmodeinfo.cwidth * (depth / 8);

        for (int col = 0; col < cols * CBmodeinfo.cwidth; col++) {
            if (depth == 32) {
                ((u32*)base)[col] = cbvga_palette_argb[bg];
            } else if (depth == 24) {
                u32 argb = cbvga_palette_argb[bg];
                u8 r = (argb >> 16) & 0xFF;
                u8 g = (argb >> 8)  & 0xFF;
                u8 b = (argb)       & 0xFF;
                u8 *p = base + col * 3;
                p[0] = b; p[1] = g; p[2] = r;
            } else if (depth == 16) {
                u32 argb = cbvga_palette_argb[bg];
                u8 r = (argb >> 16) & 0xFF;
                u8 g = (argb >> 8)  & 0xFF;
                u8 b = (argb)       & 0xFF;
                u16 pixel = pack_rgb16(r, g, b,
                                       CBred_size, CBgreen_size, CBblue_size,
                                       CBred_pos,  CBgreen_pos,  CBblue_pos);
                ((u16*)base)[col] = pixel;
            }
        }
    }
}

// Draw a block/line cursor by inverting pixels between start..end scanlines
void cbvga_draw_cursor(u16 cx, u16 cy, u8 attr, int start, int end) {
    int char_height = GET_BDA(char_height);
    if (end >= char_height) end = char_height - 1;
    if (start > end) return;

    int depth = GET_GLOBAL(CBmodeinfo.depth);

    for (int row = start; row <= end; row++) {
        u8 *base = (u8*)(uintptr_t)GET_GLOBAL(VBE_framebuffer)
                 + (cy*char_height + row) * GET_GLOBAL(CBlinelength)
                 + cx * CBmodeinfo.cwidth * (depth / 8);

        for (int col = 0; col < CBmodeinfo.cwidth; col++) {
            if (depth == 32) {
                // Invert RGB, keep alpha
                ((u32*)base)[col] ^= 0x00FFFFFF;
            } else if (depth == 24) {
                u8 *p = base + col * 3;
                p[0] = 0xFF - p[0]; // B
                p[1] = 0xFF - p[1]; // G
                p[2] = 0xFF - p[2]; // R
            } else if (depth == 16) {
                // Read current pixel
                u16 pix = ((u16*)base)[col];

                // Expand to 8‑bit channels using mask globals
                u8 r = ((pix >> CBred_pos)   & ((1 << CBred_size)   - 1)) << (8 - CBred_size);
                u8 g = ((pix >> CBgreen_pos) & ((1 << CBgreen_size) - 1)) << (8 - CBgreen_size);
                u8 b = ((pix >> CBblue_pos)  & ((1 << CBblue_size)  - 1)) << (8 - CBblue_size);

                // Invert RGB
                r = 0xFF - r;
                g = 0xFF - g;
                b = 0xFF - b;

                // Repack to 16‑bit
                ((u16*)base)[col] = pack_rgb16(r, g, b,
                                               CBred_size, CBgreen_size, CBblue_size,
                                               CBred_pos,  CBgreen_pos,  CBblue_pos);
            }
        }
    }
}

// Restore cell without cursor (redraw char)
// This works for both 16‑bit and 32‑bit modes because cbvga_render_char()
// handles the pixel format conversion internally.
void cbvga_hide_cursor(u16 cx, u16 cy, u8 attr) {
    struct carattr c = vgafb_read_char((struct cursorpos){cx, cy, 0, 0});
    cbvga_render_char(cx, cy, c.car, c.attr);
}

// -----------------------------------------------------------------------------
// Provide framebuffer info to text-mode emulator
// -----------------------------------------------------------------------------
int cbvga_get_fb(cbvga_fb_info *info)
{
    if (!GET_GLOBAL(VBE_framebuffer) ||
        !GET_GLOBAL(CBmodeinfo.width) ||
        !GET_GLOBAL(CBmodeinfo.height)) {
        return -1; // no framebuffer available
    }

    info->base   = (void*)(uintptr_t)GET_GLOBAL(VBE_framebuffer);
    info->width  = GET_GLOBAL(CBmodeinfo.width);
    info->height = GET_GLOBAL(CBmodeinfo.height);
    info->pitch  = GET_GLOBAL(CBlinelength);
    return 0;
}