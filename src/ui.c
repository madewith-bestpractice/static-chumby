#include "common.h"
#include "ui.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint16_t back[W * H];
static uint16_t *fbmem;
static int stride_px = W;

int ui_init(void) {
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOG("ui: open fb0 failed"); return -1; }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) return -1;
    if (v.bits_per_pixel != 16) { LOG("ui: unexpected bpp %u", v.bits_per_pixel); return -1; }
    stride_px = (int)f.line_length / 2;
    size_t len = (size_t)f.line_length * v.yres_virtual;
    fbmem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED) { LOG("ui: mmap failed"); return -1; }
    if (v.yoffset != 0) { v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v); }
    LOG("ui: fb %ux%u (virtual %ux%u) stride %d", v.xres, v.yres, v.xres_virtual, v.yres_virtual, stride_px);
    return 0;
}

void ui_present(void) {
    for (int y = 0; y < H; y++) memcpy(fbmem + y * stride_px, back + y * W, W * 2);
}

void ui_present_rect(int x, int y, int w, int h) {
    for (int r = y; r < y + h && r < H; r++) memcpy(fbmem + r * stride_px + x, back + r * W + x, (size_t)w * 2);
}

/* t=255 -> b. Packed blend: spread RGB565 into 0x07E0F81F lanes so one
 * multiply blends all three channels (5-bit alpha), ~5x the old per-channel math. */
uint16_t ui_mix(uint16_t a, uint16_t b, int t) {
    uint32_t al = (uint32_t)(t + 4) >> 3;   /* 0..32 */
    if (al >= 32) return b;
    if (!al) return a;
    uint32_t x = (a | (uint32_t)a << 16) & 0x07E0F81Fu;
    uint32_t y = (b | (uint32_t)b << 16) & 0x07E0F81Fu;
    uint32_t r = (x + (((y - x) * al) >> 5)) & 0x07E0F81Fu;
    return (uint16_t)(r | r >> 16);
}

const uint16_t *ui_back(void) { return back; }

void ui_fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = back + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void ui_fill_alpha(int x, int y, int w, int h, uint16_t c, int a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = back + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = ui_mix(p[i], c, a);
    }
}

int ui_text(const font_t *f, int x, int y, const char *s, uint16_t c) {
    int pen = x;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const glyph_t *g = &f->glyphs[ch - 32];
        int gx = pen + g->xoff, gy = y - g->yoff;
        for (int r = 0; r < g->h; r++) {
            int py = gy + r;
            if (py < 0 || py >= H) continue;
            const uint8_t *src = f->bits + g->offset + r * g->w;
            uint16_t *dst = back + py * W;
            for (int q = 0; q < g->w; q++) {
                int px = gx + q, a = src[q];
                if (a && px >= 0 && px < W) dst[px] = a > 250 ? c : ui_mix(dst[px], c, a);
            }
        }
        pen += g->adv;
    }
    return pen - x;
}

int ui_text_width(const font_t *f, const char *s) {
    int w = 0;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        w += f->glyphs[ch - 32].adv;
    }
    return w;
}

/* Copies s into out, truncated with "..." so it fits max_w. */
static void fit(const font_t *f, const char *s, char *out, int cap, int max_w) {
    snprintf(out, (size_t)cap, "%s", s);
    if (ui_text_width(f, out) <= max_w) return;
    int n = (int)strlen(out);
    while (n > 0) {
        out[n] = 0;
        while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
        char tmp[260];
        snprintf(tmp, sizeof tmp, "%s...", out);
        if (ui_text_width(f, tmp) <= max_w) { snprintf(out, (size_t)cap, "%s", tmp); return; }
        n--;
    }
}

void ui_text_center_fit(const font_t *f, int cx, int y, const char *s, int max_w, uint16_t c) {
    char buf[256];
    fit(f, s, buf, sizeof buf, max_w);
    ui_text(f, cx - ui_text_width(f, buf) / 2, y, buf, c);
}

void ui_text_left_fit(const font_t *f, int x, int y, const char *s, int max_w, uint16_t c) {
    char buf[256];
    fit(f, s, buf, sizeof buf, max_w);
    ui_text(f, x, y, buf, c);
}

void ui_ascii(const char *in, char *out, int cap) {
    int o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o < cap - 4) {
        unsigned cp = *p;
        int len = 1;
        if (cp >= 0xF0 && p[1] && p[2] && p[3]) { cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); len = 4; }
        else if (cp >= 0xE0 && p[1] && p[2]) { cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); len = 3; }
        else if (cp >= 0xC0 && p[1]) { cp = ((cp & 31) << 6) | (p[1] & 63); len = 2; }
        p += len;
        const char *rep = NULL;
        switch (cp) {
        case 0x2018: case 0x2019: case 0x02BC: rep = "'"; break;
        case 0x201C: case 0x201D: rep = "\""; break;
        case 0x2013: case 0x2014: rep = "-"; break;
        case 0x2026: rep = "..."; break;
        case 0x00A0: case 0x202F: case '\n': case '\t': rep = " "; break;
        case 0x00E9: case 0x00E8: case 0x00EA: rep = "e"; break;
        case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E4: rep = "a"; break;
        case 0x00ED: case 0x00EF: rep = "i"; break;
        case 0x00F3: case 0x00F6: case 0x00F4: rep = "o"; break;
        case 0x00FA: case 0x00FC: rep = "u"; break;
        case 0x00F1: rep = "n"; break;
        case 0x00E7: rep = "c"; break;
        }
        if (rep) { while (*rep && o < cap - 1) out[o++] = *rep++; }
        else if (cp >= 32 && cp < 127) out[o++] = (char)cp;
        /* other code points (emoji etc.) are dropped */
    }
    out[o] = 0;
}

void ui_icon(const icon_t *ic, int x, int y, uint16_t c) {
    for (int r = 0; r < ic->h; r++) {
        int py = y + r;
        if (py < 0 || py >= H) continue;
        for (int q = 0; q < ic->w; q++) {
            int px = x + q, a = ic->bits[r * ic->w + q];
            if (a && px >= 0 && px < W) back[py * W + px] = a > 250 ? c : ui_mix(back[py * W + px], c, a);
        }
    }
}

void ui_icon_rot180(const icon_t *ic, int x, int y, uint16_t c) {
    for (int r = 0; r < ic->h; r++) {
        int py = y + r;
        if (py < 0 || py >= H) continue;
        for (int q = 0; q < ic->w; q++) {
            int px = x + q, a = ic->bits[(ic->h - 1 - r) * ic->w + (ic->w - 1 - q)];
            if (a && px >= 0 && px < W) back[py * W + px] = a > 250 ? c : ui_mix(back[py * W + px], c, a);
        }
    }
}

int ui_text_wrap(const font_t *f, int x, int y, int max_w, int line_h, int max_lines, const char *s, uint16_t c) {
    char line[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        /* longest prefix that fits, broken at a space when possible */
        int n = 0, brk = -1;
        while (s[n]) {
            char tmp[256];
            int m = n + 1 < (int)sizeof tmp ? n + 1 : (int)sizeof tmp - 1;
            memcpy(tmp, s, (size_t)m); tmp[m] = 0;
            if (ui_text_width(f, tmp) > max_w) break;
            if (s[n] == ' ') brk = n;
            n++;
        }
        int take = (!s[n] || brk <= 0) ? n : brk;
        if (take <= 0) take = 1;
        if (lines == max_lines - 1 && s[take]) {   /* last line: ellipsize the rest */
            ui_text_left_fit(f, x, y + lines * line_h, s, max_w, c);
            return lines + 1;
        }
        memcpy(line, s, (size_t)take); line[take] = 0;
        ui_text(f, x, y + lines * line_h, line, c);
        s += take;
        lines++;
    }
    return lines;
}

void ui_blit_half(const uint16_t *px, int w, int h, int x, int y) {
    for (int r = 0; r < h / 2 && y + r < H; r++) {
        uint16_t *d = back + (y + r) * W + x;
        const uint16_t *a = px + (2 * r) * w, *b = a + w;
        for (int q = 0; q < w / 2 && x + q < W; q++) {
            uint16_t p0 = a[2*q], p1 = a[2*q+1], p2 = b[2*q], p3 = b[2*q+1];
            int R = ((p0 >> 11) + (p1 >> 11) + (p2 >> 11) + (p3 >> 11) + 2) >> 2;
            int G = (((p0 >> 5) & 63) + ((p1 >> 5) & 63) + ((p2 >> 5) & 63) + ((p3 >> 5) & 63) + 2) >> 2;
            int B = ((p0 & 31) + (p1 & 31) + (p2 & 31) + (p3 & 31) + 2) >> 2;
            d[q] = (uint16_t)(R << 11 | G << 5 | B);
        }
    }
}

void ui_read_rect(uint16_t *px, int x, int y, int w, int h) {
    for (int r = 0; r < h && y + r < H; r++) memcpy(px + r * w, back + (y + r) * W + x, (size_t)w * 2);
}

void ui_blend_rect(const uint16_t *px, int x, int y, int w, int h, int a) {
    if (a <= 0) return;
    for (int r = 0; r < h && y + r < H; r++) {
        uint16_t *d = back + (y + r) * W + x;
        const uint16_t *src = px + r * w;
        for (int q = 0; q < w; q++) d[q] = ui_mix(d[q], src[q], a);
    }
}

void ui_triangle_up(int cx, int y, int half, uint16_t c) {
    for (int r = 0; r <= half; r++) ui_fill(cx - r, y + r, 2 * r + 1, 1, c);
}

void ui_blit_rect(const uint16_t *px, int x, int y, int w, int h) {
    for (int r = 0; r < h && y + r < H; r++) memcpy(back + (y + r) * W + x, px + r * w, (size_t)w * 2);
}

void ui_triangle_down(int cx, int y, int half, uint16_t c) {
    for (int r = 0; r <= half; r++) ui_fill(cx - (half - r), y + r, 2 * (half - r) + 1, 1, c);
}
