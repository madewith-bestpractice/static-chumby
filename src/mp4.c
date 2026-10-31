#include "mp4.h"
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) << 32 | u32(p + 4); }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Iterate boxes in [p, end): calls back with type, payload, payload length. */
typedef int (*box_cb)(uint32_t type, const uint8_t *body, size_t len, void *ctx);
static int each_box(const uint8_t *p, const uint8_t *end, box_cb cb, void *ctx) {
    while (p + 8 <= end) {
        uint64_t size = u32(p);
        uint32_t type = u32(p + 4);
        size_t hdr = 8;
        if (size == 1) { if (p + 16 > end) return -1; size = u64(p + 8); hdr = 16; }
        else if (size == 0) size = (uint64_t)(end - p);
        if (size < hdr || p + size > end) return -1;
        int r = cb(type, p + hdr, (size_t)size - hdr, ctx);
        if (r) return r;
        p += size;
    }
    return 0;
}

#define T(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct { mp4idx_t *m; int is_sound; uint32_t timescale; const uint8_t *stbl; size_t stbl_len; } trak_ctx;

/* descriptor length: up to 4 bytes of 7 bits */
static size_t desc_len(const uint8_t **p, const uint8_t *end) {
    size_t n = 0;
    for (int i = 0; i < 4 && *p < end; i++) { uint8_t b = *(*p)++; n = n << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    return n;
}

static void parse_esds(const uint8_t *b, size_t len, mp4idx_t *m) {
    const uint8_t *p = b + 4, *end = b + len;   /* skip version/flags */
    while (p + 2 <= end) {
        uint8_t tag = *p++;
        size_t l = desc_len(&p, end);
        if (tag == 0x03) {                       /* ES_Descriptor */
            if (p + 3 > end) return;
            uint8_t flags = p[2];
            p += 3;
            if (flags & 0x80) p += 2;
            if (flags & 0x40) p += 1 + *p;
            if (flags & 0x20) p += 2;
            continue;                            /* children follow inline */
        }
        if (tag == 0x04) { p += 13; continue; }  /* DecoderConfigDescriptor header, then children */
        if (tag == 0x05) {                       /* DecoderSpecificInfo = AudioSpecificConfig */
            if (l > sizeof m->asc || p + l > end) return;
            memcpy(m->asc, p, l);
            m->asc_len = (int)l;
            return;
        }
        p += l;
    }
}

static int asc_bits(const mp4idx_t *m, int *pos, int n) {
    int v = 0;
    for (int i = 0; i < n; i++, (*pos)++) {
        int byte = *pos >> 3;
        int bit = byte < m->asc_len ? (m->asc[byte] >> (7 - (*pos & 7))) & 1 : 0;
        v = v << 1 | bit;
    }
    return v;
}

static void parse_asc(mp4idx_t *m) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    if (m->asc_len < 2) return;
    int pos = 0;
    int aot = asc_bits(m, &pos, 5);
    if (aot == 31) aot = 32 + asc_bits(m, &pos, 6);
    int sfi = asc_bits(m, &pos, 4);
    int rate = sfi == 15 ? asc_bits(m, &pos, 24) : sfi < 13 ? rates[sfi] : 0;
    int ch = asc_bits(m, &pos, 4);
    if (aot == 5 || aot == 29) {           /* explicit SBR/PS: core config follows */
        int esfi = asc_bits(m, &pos, 4);
        if (esfi == 15) asc_bits(m, &pos, 24);
        aot = asc_bits(m, &pos, 5);
    }
    m->aot = aot;
    m->channels = ch;
    m->core_rate = rate;
}

static int stsd_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    trak_ctx *t = ctx;
    if (type != T('m', 'p', '4', 'a') || len < 28) return 0;
    uint16_t ver = u16(b + 8);                     /* QuickTime sound description version */
    size_t skip = 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
    if (skip > len) return 0;
    const uint8_t *p = b + skip, *end = b + len;
    while (p + 8 <= end) {                         /* child boxes: find esds */
        uint32_t sz = u32(p), ty = u32(p + 4);
        if (sz < 8 || p + sz > end) break;
        if (ty == T('e', 's', 'd', 's')) parse_esds(p + 8, sz - 8, t->m);
        p += sz;
    }
    return 0;
}

static int stbl_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    mp4idx_t *m = ((trak_ctx *)ctx)->m;
    if (type == T('s', 't', 's', 'd') && len > 8) each_box(b + 8, b + len, stsd_cb, ctx);
    else if (type == T('s', 't', 't', 's') && len >= 8) {
        uint32_t n = u32(b + 4);
        if (8 + (uint64_t)n * 8 > len) return -1;
        m->n_stts = n;
        m->stts_count = malloc(n * 4 + 4); m->stts_delta = malloc(n * 4 + 4);
        for (uint32_t i = 0; i < n; i++) { m->stts_count[i] = u32(b + 8 + i * 8); m->stts_delta[i] = u32(b + 12 + i * 8); }
    } else if (type == T('s', 't', 's', 'c') && len >= 8) {
        uint32_t n = u32(b + 4);
        if (8 + (uint64_t)n * 12 > len) return -1;
        m->n_stsc = n;
        m->stsc_first = malloc(n * 4 + 4); m->stsc_spc = malloc(n * 4 + 4);
        for (uint32_t i = 0; i < n; i++) { m->stsc_first[i] = u32(b + 8 + i * 12); m->stsc_spc[i] = u32(b + 12 + i * 12); }
    } else if (type == T('s', 't', 's', 'z') && len >= 12) {
        m->uniform_size = u32(b + 4);
        m->nsamples = u32(b + 8);
        if (!m->uniform_size) {
            if (12 + (uint64_t)m->nsamples * 4 > len) return -1;
            m->sizes = malloc((size_t)m->nsamples * 4 + 4);
            for (uint32_t i = 0; i < m->nsamples; i++) m->sizes[i] = u32(b + 12 + i * 4);
        }
    } else if ((type == T('s', 't', 'c', 'o') || type == T('c', 'o', '6', '4')) && len >= 8) {
        int wide = type == T('c', 'o', '6', '4');
        uint32_t n = u32(b + 4);
        if (8 + (uint64_t)n * (wide ? 8 : 4) > len) return -1;
        m->n_chunks = n;
        m->chunk_off = malloc((size_t)n * 8 + 8);
        for (uint32_t i = 0; i < n; i++) m->chunk_off[i] = wide ? u64(b + 8 + i * 8) : u32(b + 8 + i * 4);
    }
    return 0;
}

static int mdia_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx);
static int minf_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    trak_ctx *t = ctx;
    if (type == T('s', 't', 'b', 'l')) { t->stbl = b; t->stbl_len = len; }
    return 0;
}
static int mdia_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    trak_ctx *t = ctx;
    if (type == T('m', 'd', 'h', 'd') && len >= 24) t->timescale = b[0] == 1 ? u32(b + 20) : u32(b + 12);
    else if (type == T('h', 'd', 'l', 'r') && len >= 12) t->is_sound = u32(b + 8) == T('s', 'o', 'u', 'n');
    else if (type == T('m', 'i', 'n', 'f')) each_box(b, b + len, minf_cb, ctx);
    return 0;
}
static int trak_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    if (type == T('m', 'd', 'i', 'a')) each_box(b, b + len, mdia_cb, ctx);
    return 0;
}
static int moov_cb(uint32_t type, const uint8_t *b, size_t len, void *ctx) {
    mp4idx_t *m = ctx;
    if (type != T('t', 'r', 'a', 'k') || m->n_chunks) return 0;   /* first sound track wins */
    trak_ctx t = { .m = m };
    each_box(b, b + len, trak_cb, &t);
    if (!t.is_sound || !t.stbl) return 0;
    m->timescale = t.timescale;
    each_box(t.stbl, t.stbl + t.stbl_len, stbl_cb, &t);
    if (!m->asc_len) { mp4_free(m); memset(m, 0, sizeof *m); }   /* not AAC: keep looking */
    return 0;
}

int mp4_parse_moov(const uint8_t *moov, size_t len, mp4idx_t *m) {
    memset(m, 0, sizeof *m);
    if (len < 8 || u32(moov + 4) != T('m', 'o', 'o', 'v')) return -1;
    each_box(moov + 8, moov + len, moov_cb, m);
    if (!m->n_chunks || !m->n_stsc || !m->n_stts || !m->nsamples || !m->asc_len || !m->timescale) { mp4_free(m); return -1; }
    parse_asc(m);
    return m->core_rate && m->channels ? 0 : -1;
}

void mp4_free(mp4idx_t *m) {
    free(m->stts_count); free(m->stts_delta); free(m->sizes);
    free(m->stsc_first); free(m->stsc_spc); free(m->chunk_off);
    m->stts_count = m->stts_delta = m->sizes = m->stsc_first = m->stsc_spc = NULL;
    m->chunk_off = NULL;
}

uint32_t mp4_sample_for_ms(const mp4idx_t *m, int64_t ms, int64_t *sample_ms) {
    uint64_t t = ms > 0 ? (uint64_t)ms * m->timescale / 1000 : 0, start = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->n_stts; i++) {
        uint64_t run = (uint64_t)m->stts_count[i] * m->stts_delta[i];
        if (t < run && m->stts_delta[i]) {
            uint32_t j = (uint32_t)((t + m->stts_delta[i] - 1) / m->stts_delta[i]);   /* at or after */
            k += j;
            start += (uint64_t)j * m->stts_delta[i];
            break;
        }
        t -= run; k += m->stts_count[i]; start += run;
    }
    if (k >= m->nsamples) k = m->nsamples ? m->nsamples - 1 : 0;
    if (sample_ms) *sample_ms = (int64_t)(start * 1000 / m->timescale);
    return k;
}

static uint32_t size_of(const mp4idx_t *m, uint32_t k) { return m->uniform_size ? m->uniform_size : m->sizes[k]; }

int mp4_iter_seek(const mp4idx_t *m, uint32_t k, mp4iter_t *it) {
    if (k >= m->nsamples) return -1;
    uint32_t left = k;
    for (uint32_t i = 0; i < m->n_stsc; i++) {
        uint32_t first = m->stsc_first[i], next = i + 1 < m->n_stsc ? m->stsc_first[i + 1] : m->n_chunks + 1;
        uint64_t run = (uint64_t)(next - first) * m->stsc_spc[i];
        if (left < run) {
            it->chunk = first + left / m->stsc_spc[i];
            it->in_chunk = left % m->stsc_spc[i];
            it->stsc_i = i;
            it->k = k;
            if (it->chunk - 1 >= m->n_chunks) return -1;
            it->off = m->chunk_off[it->chunk - 1];
            for (uint32_t s = k - it->in_chunk; s < k; s++) it->off += size_of(m, s);
            return 0;
        }
        left -= (uint32_t)run;
    }
    return -1;
}

int mp4_iter_next(const mp4idx_t *m, mp4iter_t *it, uint64_t *off, uint32_t *size) {
    if (it->k >= m->nsamples || it->chunk - 1 >= m->n_chunks) return 0;
    *off = it->off;
    *size = size_of(m, it->k);
    it->k++;
    it->in_chunk++;
    if (it->in_chunk >= m->stsc_spc[it->stsc_i]) {   /* next chunk */
        it->chunk++;
        it->in_chunk = 0;
        if (it->stsc_i + 1 < m->n_stsc && it->chunk >= m->stsc_first[it->stsc_i + 1]) it->stsc_i++;
        if (it->chunk - 1 < m->n_chunks) it->off = m->chunk_off[it->chunk - 1];
    } else it->off += *size;
    return 1;
}
