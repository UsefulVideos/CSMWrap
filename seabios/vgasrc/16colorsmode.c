// 16colorsmode.c
// Emulated VGA text mode (INT 10h, mode 03h) for UEFI Class 3 via CSMWrap.
// Renders 80x25 text using SeaBIOS's 8x16 font (vgafonts.c) into cbvga framebuffer
// and maps 16-color attributes via cbvga_palette_emulator.
//
// Integrates with SeaBIOS-style INT 10h dispatcher (handle_10) by providing
// tm_handle_int10(struct bregs *regs). Call tm_init() during video init,
// and tm_enable() either on AH=00h/AL=03h or at boot if desired.

#include "../src/bregs.h"
#include "../src/biosvar.h"
#include "../src/output.h"
#include "vgabios.h"
#include "vgautil.h"
#include "cbvga_shared.h"

// bring in mask globals and packer from cbvga.c
extern u8 CBred_size, CBgreen_size, CBblue_size;
extern u8 CBred_pos,  CBgreen_pos,  CBblue_pos;
u16 pack_rgb16(u8 r, u8 g, u8 b,
               u8 rsize, u8 gsize, u8 bsize,
               u8 rpos,  u8 gpos,  u8 bpos);

#include "cbvga_palette_emulator.h"
#include "vgafonts.h"
#include "16colorsmode.h"

#define TM_COLS   80
#define TM_ROWS   25
#define GLYPH_W   8
#define GLYPH_H   16

typedef struct {
    u8 ch;
    u8 attr;  // low nibble: fg (0..15), high nibble: bg (0..7), bit7: blink (ignored)
} tm_cell_t;

// State
static tm_cell_t tm_buf[TM_ROWS][TM_COLS];
static int tm_row = 0, tm_col = 0;
static u8  tm_attr = 0x07;                // light gray on black
static u8  cursor_start = 0x0D, cursor_end = 0x0F;
static int tm_active = 0;

// Framebuffer via cbvga
static u8  *fb_ptr = NULL;
static u32  fb_w = 0, fb_h = 0, fb_pitch = 0;
static int  fb_bpp = 32;

// Text region origin (centered)
static int origin_x = 0, origin_y = 0;

// Cached 16-color palette (XRGB8888)
static u32 rgb_cache[16];

static inline void tm_refresh_palette_cache(void) {
    for (int i = 0; i < 16; i++)
        rgb_cache[i] = cbvga_palette_lookup(i);
}

static inline void putpx(int x, int y, u32 argb) {
    if ((u32)x >= fb_w || (u32)y >= fb_h) return;
    u8 *row = fb_ptr + y * fb_pitch;

    if (fb_bpp == 32) {
        ((u32*)row)[x] = argb;
    } else if (fb_bpp == 24) {
        u8 *p = row + x * 3;
        p[0] = (u8)(argb      & 0xFF); // B
        p[1] = (u8)((argb>>8) & 0xFF); // G
        p[2] = (u8)((argb>>16)& 0xFF); // R
    } else if (fb_bpp == 16) {
        u8 r = (argb >> 16) & 0xFF;
        u8 g = (argb >> 8)  & 0xFF;
        u8 b = (argb      ) & 0xFF;
        ((u16*)row)[x] = pack_rgb16(r,g,b,
                                    CBred_size,CBgreen_size,CBblue_size,
                                    CBred_pos, CBgreen_pos, CBblue_pos);
    }
}

static void render_cell(int r, int c) {
    tm_cell_t cell = tm_buf[r][c];
    const u8 *glyph = font_8x16[cell.ch];
    int fg = cell.attr & 0x0F;
    int bg = (cell.attr >> 4) & 0x07;
    u32 fg_rgb = rgb_cache[fg];
    u32 bg_rgb = rgb_cache[bg];

    int x0 = origin_x + c * GLYPH_W;
    int y0 = origin_y + r * GLYPH_H;

    for (int gy = 0; gy < GLYPH_H; gy++) {
        u8 bits = glyph[gy];
        for (int gx = 0; gx < GLYPH_W; gx++) {
            int on = !!(bits & (0x80 >> gx));
            putpx(x0 + gx, y0 + gy, on ? fg_rgb : bg_rgb);
        }
    }
}

static void render_cursor(void) {
    int x0 = origin_x + tm_col * GLYPH_W;
    int y0 = origin_y + tm_row * GLYPH_H + cursor_start;
    int y1 = origin_y + tm_row * GLYPH_H + cursor_end;
    u32 fg_rgb = rgb_cache[tm_attr & 0x0F];

    if (y0 < origin_y) y0 = origin_y;
    if (y1 >= origin_y + GLYPH_H) y1 = origin_y + GLYPH_H - 1;

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x < x0 + GLYPH_W; x++)
            putpx(x, y, fg_rgb);
}

static void mark_all_dirty(void) {
    for (int r = 0; r < TM_ROWS; r++)
        for (int c = 0; c < TM_COLS; c++)
            render_cell(r, c);
    render_cursor();
}

static void tm_clear(u8 fill_attr) {
    for (int r = 0; r < TM_ROWS; r++)
        for (int c = 0; c < TM_COLS; c++) {
            tm_buf[r][c].ch   = ' ';
            tm_buf[r][c].attr = fill_attr;
        }
}

static void tm_scroll_up(int lines, int r0, int c0, int r1, int c1, u8 attr) {
    if (lines <= 0) return;
    if (r0 < 0) r0 = 0;
	if (c0 < 0) c0 = 0;
    if (r1 >= TM_ROWS) r1 = TM_ROWS - 1;
    if (c1 >= TM_COLS) c1 = TM_COLS - 1;

    for (int r = r0; r <= r1 - lines; r++)
        for (int c = c0; c <= c1; c++)
            tm_buf[r][c] = tm_buf[r + lines][c];

    for (int r = r1 - lines + 1; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            tm_buf[r][c].ch   = ' ';
            tm_buf[r][c].attr = attr;
        }
}

static void tm_putch(u8 ch) {
    switch (ch) {
    case '\r': tm_col = 0; return;
    case '\n':
        tm_row++;
        if (tm_row >= TM_ROWS) {
            tm_row = TM_ROWS - 1;
            tm_scroll_up(1, 0, 0, TM_ROWS - 1, TM_COLS - 1, tm_attr);
            mark_all_dirty();
        }
        return;
    case '\b':
        if (tm_col > 0) tm_col--;
        return;
    case '\t': {
        int next = ((tm_col / 8) + 1) * 8;
        if (next >= TM_COLS) { tm_col = 0; tm_putch('\n'); }
        else tm_col = next;
        return;
    }
    default:
        tm_buf[tm_row][tm_col].ch   = ch;
        tm_buf[tm_row][tm_col].attr = tm_attr;
        render_cell(tm_row, tm_col);
        tm_col++;
        if (tm_col >= TM_COLS) { tm_col = 0; tm_putch('\n'); }
        return;
    }
}

// Public API
void tm_init(void) {
    cbvga_fb_info fbinfo;
    if (cbvga_get_fb(&fbinfo) != 0) {
        dprintf(1, "TextModeEmu: cbvga_get_fb failed\n");
        tm_active = 0;
        return;
    }
    fb_ptr   = fbinfo.base;
    fb_w     = fbinfo.width;
    fb_h     = fbinfo.height;
    fb_pitch = fbinfo.pitch;

    // Derive bits‑per‑pixel from pitch/width if not provided
    unsigned bytes_per_px = fb_pitch / fb_w;
    if (bytes_per_px == 4)
        fb_bpp = 32;
    else if (bytes_per_px == 3)
        fb_bpp = 24;
    else if (bytes_per_px == 2)
        fb_bpp = 16;
    else
        fb_bpp = 32; // safe default

    origin_x = (int)(fb_w - TM_COLS * GLYPH_W) / 2;
    origin_y = (int)(fb_h - TM_ROWS * GLYPH_H) / 2;

    cbvga_palette_set_entries(NULL, 16);
    tm_refresh_palette_cache();

    tm_active = 0;
    dprintf(1, "TextModeEmu: init fb=%p %ux%u pitch=%u bpp=%d origin=(%d,%d)\n",
            fb_ptr, fb_w, fb_h, fb_pitch, fb_bpp, origin_x, origin_y);
}

void tm_enable(void) {
    tm_active = 1;
    tm_row = tm_col = 0;
    tm_attr = 0x07;
    cursor_start = 0x0D; cursor_end = 0x0F;

    tm_clear(0x07);
    mark_all_dirty();

    dprintf(1, "TextModeEmu: enabled (80x25)\n");
}

void tm_disable(void) {
    tm_active = 0;
    dprintf(1, "TextModeEmu: disabled\n");
}

int tm_handle_int10(struct bregs *regs) {
    // If we’re inactive, only intercept Set Video Mode 03h
    if (!tm_active && AH(regs) == 0x00 && AL(regs) == 0x03) {
        tm_enable();
        SET_AH(regs, 0x00);
        return 1;
    }
    if (!tm_active)
        return 0;

    switch (AH(regs)) {
    case 0x00: // Set video mode
        if (AL(regs) == 0x03) {
            tm_enable();
            SET_AH(regs, 0x00);
            return 1;
        }
        return 0;

    case 0x01: // Set cursor shape
        cursor_start = CH(regs) & 0x1F;
        cursor_end   = CL(regs) & 0x1F;
        SET_AH(regs, 0x00);
        return 1;

    case 0x02: // Set cursor position
        tm_row = (DH(regs) < TM_ROWS) ? DH(regs) : TM_ROWS - 1;
        tm_col = (DL(regs) < TM_COLS) ? DL(regs) : TM_COLS - 1;
        SET_AH(regs, 0x00);
        return 1;

    case 0x03: // Get cursor position/shape
        regs->bh = 0;
        regs->dh = tm_row;
        regs->dl = tm_col;
        regs->cx = (cursor_end << 8) | cursor_start;
        SET_AH(regs, 0x00);
        return 1;

    case 0x05: // Select active page (stubbed)
        SET_AH(regs, 0x00);
        return 1;

    case 0x06: { // Scroll up
        u8 lines = AL(regs) ? AL(regs) : TM_ROWS;
        u8 attr  = BH(regs);
        tm_scroll_up(lines, CH(regs), CL(regs), DH(regs), DL(regs), attr);
        mark_all_dirty();
        SET_AH(regs, 0x00);
        return 1;
    }

    case 0x08: // Read char/attr at cursor
        SET_AL(regs, tm_buf[tm_row][tm_col].ch);
        SET_AH(regs, tm_buf[tm_row][tm_col].attr);
        return 1;

    case 0x09: { // Write char/attr at cursor
        u8 ch   = AL(regs);
        u8 attr = BL(regs) ? BL(regs) : tm_attr;
        int count = regs->cx ? regs->cx : 1;
        tm_attr = attr;
        while (count--)
            tm_putch(ch);
        SET_AH(regs, 0x00);
        return 1;
    }

    case 0x0A: { // Write char at cursor (no attribute change)
        u8 ch = AL(regs);
        int count = regs->cx ? regs->cx : 1;
        while (count--)
            tm_putch(ch);
        SET_AH(regs, 0x00);
        return 1;
    }

    case 0x0E: // Teletype output
        tm_putch(AL(regs));
        SET_AH(regs, 0x00);
        return 1;

    case 0x0F: // Get current video mode
        SET_AL(regs, 0x03);
        SET_AH(regs, TM_COLS);
        regs->bh = 0;
        return 1;

    case 0x13: { // Write string
        // AL: mode (bit0: update cursor), BL: attr, BH: page (ignored)
        // CX: length, ES:BP -> string, DH: row, DL: col
        int update_cursor = AL(regs) & 0x01;
        u8 attr = BL(regs) ? BL(regs) : tm_attr;
        u16 seg = regs->es, off = regs->bp;
        int len = regs->cx;
        int row = (DH(regs) < TM_ROWS) ? DH(regs) : TM_ROWS - 1;
        int col = (DL(regs) < TM_COLS) ? DL(regs) : TM_COLS - 1;

        tm_row = row;
        tm_col = col;
        tm_attr = attr;

        while (len--) {
            u8 ch = GET_FARVAR(seg, *(u8*)(off++));
            tm_putch(ch);
        }

        if (!update_cursor) {
            // Optionally restore cursor to original position if desired
        }

        SET_AH(regs, 0x00);
        return 1;
    }

    default:
        return 0;
    }
}