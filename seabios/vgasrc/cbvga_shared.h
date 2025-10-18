// cbvga_shared.h
// Shared definitions between cbvga.c and 16colorsmode.c
// Licensed under GNU LGPLv3

#ifndef CBVGA_SHARED_H
#define CBVGA_SHARED_H

#include <stdint.h>

// Framebuffer info structure
typedef struct {
    void *base;        // linear framebuffer base address
    unsigned width;    // width in pixels
    unsigned height;   // height in pixels
    unsigned pitch;    // bytes per scanline
} cbvga_fb_info;

// Accessor implemented in cbvga.c
int cbvga_get_fb(cbvga_fb_info *info);

#endif // CBVGA_SHARED_H