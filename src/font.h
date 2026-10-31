#pragma once
#include <stdint.h>

typedef struct {
    uint8_t w, h;      /* bitmap size */
    int8_t xoff;       /* left edge relative to the pen x */
    int8_t yoff;       /* top edge, pixels ABOVE the baseline */
    uint8_t adv;       /* pen advance */
    uint32_t offset;   /* into the face's bitmap array */
} glyph_t;

typedef struct {
    int px, ascent, descent;
    const glyph_t *glyphs;   /* ASCII 32..126 */
    const uint8_t *bits;     /* 8-bit alpha, row-major, top row first */
} font_t;
