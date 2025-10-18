// cbvga_palette_emulator.h
// Bootstrap header for CSMWrap palette support.
// Provides globals and a simple initializer; full emulator lives in vgasrc.

#ifndef __CBVGA_PALETTE_EMULATOR_H
#define __CBVGA_PALETTE_EMULATOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Local type aliases (guarded to avoid redefinition)
// -----------------------------------------------------------------------------
#ifndef __CBVGA_TYPES_DEFINED
#define __CBVGA_TYPES_DEFINED
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

// -----------------------------------------------------------------------------
// Raw VGA DAC (256 x RGB) and ARGB lookup table
// -----------------------------------------------------------------------------
extern u8  cbvga_palette[256][3];
extern u32 cbvga_palette_argb[256];

// -----------------------------------------------------------------------------
// Bootstrap helper
// -----------------------------------------------------------------------------

// Reset or set the first N palette entries.
// If 'entries' is NULL or count <= 0, load the default 16‑color VGA palette.
void cbvga_palette_set_entries(const void *entries, int count);

// Convenience inline: return ARGB value for a given VGA color index (0–255).
static inline u32 cbvga_palette_lookup(int idx) {
    return cbvga_palette_argb[idx & 0xFF];
}

// -----------------------------------------------------------------------------
// Diagnostic dump (called by csmwrap.c after SeaVGABIOS dispatch)
// -----------------------------------------------------------------------------
void cbvga_emul_dump_state(void);

#ifdef __cplusplus
}
#endif

#endif // __CBVGA_PALETTE_EMULATOR_H