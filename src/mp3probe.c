#include "mp3probe.h"
#include <string.h>

int64_t mp3_id3_size(const uint8_t *b, size_t len) {
    if (len < 10 || memcmp(b, "ID3", 3)) return 0;
    if ((b[6] | b[7] | b[8] | b[9]) & 0x80) return 0;   /* not syncsafe: not a real tag */
    int64_t sz = ((int64_t)b[6] << 21) | (b[7] << 14) | (b[8] << 7) | b[9];
    return 10 + sz + ((b[5] & 0x10) ? 10 : 0);         /* + footer */
}

static const int k_br[2][16] = {   /* kbps, layer III: [MPEG1][idx], [MPEG2/2.5][idx] */
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
};
static const int k_sr[3][3] = { {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000} };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int mp3_parse_first_frame(const uint8_t *b, size_t len, mp3info_t *o) {
    /* find a frame sync (allow a little junk before the first frame) */
    for (size_t i = 0; i + 4 < len && i < 8192; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (b[i + 1] >> 3) & 3;          /* 3=MPEG1 2=MPEG2 0=MPEG2.5 */
        int layer = (b[i + 1] >> 1) & 3;        /* 1 = layer III */
        int bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3, chan = b[i + 3] >> 6;
        if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;
        int mpeg1 = ver == 3;
        o->bitrate_kbps = k_br[mpeg1 ? 0 : 1][bri];
        o->srate = k_sr[mpeg1 ? 0 : (ver == 2 ? 1 : 2)][sri];
        o->samples_per_frame = mpeg1 ? 1152 : 576;
        o->audio_start += (int64_t)i;
        /* Xing/Info sits after the side info: MPEG1 stereo 32, mono 17; MPEG2 stereo 17, mono 9 */
        size_t side = mpeg1 ? (chan == 3 ? 17 : 32) : (chan == 3 ? 9 : 17);
        size_t x = i + 4 + side;
        if (x + 8 <= len && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4))) {
            int vbr = !memcmp(b + x, "Xing", 4);
            uint32_t flags = be32(b + x + 4);
            size_t q = x + 8;
            if ((flags & 1) && q + 4 <= len) { o->frames = be32(b + q); q += 4; }
            if ((flags & 2) && q + 4 <= len) { o->xing_bytes = be32(b + q); q += 4; }
            if (vbr && (flags & 4) && q + 100 <= len) { memcpy(o->toc, b + q, 100); o->has_toc = 1; }
        }
        o->ok = o->bitrate_kbps > 0;
        return o->ok;
    }
    return 0;
}

int64_t mp3_byte_for_ms(const mp3info_t *i, int64_t ms) {
    if (!i->ok || ms <= 0) return i->audio_start;
    int64_t audio_bytes = i->xing_bytes ? i->xing_bytes : (i->total > i->audio_start ? i->total - i->audio_start : 0);
    if (i->has_toc && i->frames > 0 && i->srate > 0 && audio_bytes > 0) {
        /* VBR: interpolate the 100-point Xing table */
        double dur_ms = (double)i->frames * i->samples_per_frame * 1000.0 / i->srate;
        double pct = ms * 100.0 / dur_ms;
        if (pct >= 100.0) return i->audio_start + audio_bytes;
        int a = (int)pct;
        double fa = i->toc[a], fb = a < 99 ? i->toc[a + 1] : 256.0;
        double x = fa + (fb - fa) * (pct - a);
        return i->audio_start + (int64_t)(x / 256.0 * audio_bytes);
    }
    /* CBR (or VBR without a table: the first frame's bitrate is the best guess) */
    return i->audio_start + ms * i->bitrate_kbps / 8;   /* kbps * ms / 8 = bytes */
}
