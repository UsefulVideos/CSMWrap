// seabios/vgasrc/16colorsmode.h
// Interface for emulated VGA text mode (INT 10h, mode 03h)
// Used by CSMWrap to provide NTLDR with a working text console.

#pragma once

#include "../src/bregs.h"   // struct bregs definition from seabios/src/

// Initialize text mode emulator.
// Acquires framebuffer info from cbvga and primes palette cache.
void tm_init(void);

// Enable emulated text mode (80x25).
// Clears buffer, resets cursor/attributes, and renders initial screen.
void tm_enable(void);

// Disable text mode emulator.
// After this, tm_handle_int10() will return 0 and let normal handlers run.
void tm_disable(void);

// INT 10h handler hook.
// Returns 1 if the call was handled by the text mode emulator,
// 0 if not handled (caller should fall back to normal VGA/VBE paths).
int tm_handle_int10(struct bregs *regs);