// rep-stdio.h
// Minimal stdio replacement header for CSMWrap bootstrap code.
// Provides prototypes for the libc/nanoprintf functions we actually use.

#ifndef REP_STDIO_H
#define REP_STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

// Declare printf (implemented in libc.c / nanoprintf)
int printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // REP_STDIO_H