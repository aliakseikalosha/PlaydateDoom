#ifndef DGPD_DITHER_H
#define DGPD_DITHER_H

#include <stdint.h>

// Converts Doom's current DOOMGENERIC_RESX x DOOMGENERIC_RESY 8-bit palette
// frame (DG_ScreenBuffer) to 1bpp, writing DOOMGENERIC_RESX/8 bytes per row
// starting at dst, dst_stride bytes apart. Picks the dither pattern via
// ditherMode (see m_menu.c's Options menu) and rebuilds its palette-derived
// tables first if the palette changed.
void dgpd_dither_ConvertFrame(uint8_t *dst, int dst_stride);

#endif
