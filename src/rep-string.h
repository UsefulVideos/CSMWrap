// rep-string.h
// Minimal replacement for <string.h> in a freestanding build.
// Only declares prototypes; implementations come from libc.c.

#ifndef __REP_STRING_H
#define __REP_STRING_H

#include <stddef.h>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

// Memory functions
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

// String functions (optional, extend as needed)
size_t strlen(const char *s);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif // __REP_STRING_H