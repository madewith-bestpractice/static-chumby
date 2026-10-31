#pragma once
#include <stdint.h>
#include "font.h"

#define W 320
#define H 240

int ui_init(void);
void ui_present(void);
void ui_present_rect(int x, int y, int w, int h);
void ui_fill(int x, int y, int w, int h, uint16_t c);
void ui_fill_alpha(int x, int y, int w, int h, uint16_t c, int a);
/* Draws ASCII text with its baseline at y; returns the advance width. */
int ui_text(const font_t *f, int x, int y, const char *s, uint16_t c);
int ui_text_width(const font_t *f, const char *s);
/* Truncates (with "...") to fit max_w, centered on cx. */
void ui_text_center_fit(const font_t *f, int cx, int y, const char *s, int max_w, uint16_t c);
void ui_text_left_fit(const font_t *f, int x, int y, const char *s, int max_w, uint16_t c);
/* UTF-8 -> printable ASCII (curly quotes, dashes, ellipses mapped). */
void ui_ascii(const char *in, char *out, int cap);
uint16_t ui_mix(uint16_t a, uint16_t b, int t255);
/* The back buffer (W x H), for fast sampling. */
const uint16_t *ui_back(void);
void ui_triangle_down(int cx, int y, int half, uint16_t c);
void ui_triangle_up(int cx, int y, int half, uint16_t c);
/* RGB565 image (w x h, tightly packed) into the back buffer at (x, y). */
void ui_blit_rect(const uint16_t *px, int x, int y, int w, int h);
/* 2:1 box-filtered downsample of a w x h RGB565 image, drawn at (x, y). */
void ui_blit_half(const uint16_t *px, int w, int h, int x, int y);
/* Copy a back-buffer rect out (w x h, tightly packed). */
void ui_read_rect(uint16_t *px, int x, int y, int w, int h);
/* Blend an image over the back buffer at opacity a (0..255). */
void ui_blend_rect(const uint16_t *px, int x, int y, int w, int h, int a);
typedef struct icon_t { uint8_t w, h; const uint8_t *bits; } icon_t;
void ui_icon(const icon_t *ic, int x, int y, uint16_t c);
void ui_icon_rot180(const icon_t *ic, int x, int y, uint16_t c);
/* Word-wraps left-aligned text from (x, baseline y); returns lines used. */
int ui_text_wrap(const font_t *f, int x, int y, int max_w, int line_h, int max_lines, const char *s, uint16_t c);
