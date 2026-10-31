#include "resample.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 44100
#define TAPS 16
#define PHASES 128

struct rs {
    int in_rate;
    int passthrough;
    int16_t coef[PHASES][TAPS];   /* Q14 */
    int16_t hist[TAPS * 2][2];    /* last input frames (stereo), double-length window */
    int nhist;                    /* frames held */
    uint32_t frac;                /* position between input frames, in units of 1/OUT_RATE */
};

rs_t *rs_new(int in_rate) {
    rs_t *r = calloc(1, sizeof *r);
    r->in_rate = in_rate;
    r->passthrough = in_rate == OUT_RATE;
    if (r->passthrough) return r;
    /* cutoff at the lower Nyquist, a little inside it */
    double fc = 0.45 * (in_rate < OUT_RATE ? 1.0 : (double)OUT_RATE / in_rate);
    for (int p = 0; p < PHASES; p++) {
        double off = (double)p / PHASES, sum = 0, c[TAPS];
        for (int t = 0; t < TAPS; t++) {
            double x = t - (TAPS / 2 - 1) - off;           /* distance from the output instant */
            double s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            double w = 0.42 + 0.5 * cos(M_PI * x / (TAPS / 2)) + 0.08 * cos(2 * M_PI * x / (TAPS / 2));  /* Blackman */
            if (fabs(x) >= TAPS / 2) w = 0;
            c[t] = s * w;
            sum += c[t];
        }
        for (int t = 0; t < TAPS; t++) r->coef[p][t] = (int16_t)lrint(c[t] / sum * 16384.0);   /* unity gain */
    }
    return r;
}

void rs_free(rs_t *r) { free(r); }

static int16_t clip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

int rs_push(rs_t *r, const int16_t *in, int frames, int ch, int16_t *out, int out_cap) {
    int n = 0;
    if (r->passthrough) {
        for (int i = 0; i < frames && n < out_cap; i++, n++) {
            out[2 * n] = in[i * ch];
            out[2 * n + 1] = in[i * ch + (ch > 1)];
        }
        return n;
    }
    for (int i = 0; i < frames; i++) {
        /* append one input frame to the window */
        if (r->nhist == TAPS * 2) { memmove(r->hist, r->hist + TAPS, sizeof(r->hist[0]) * TAPS); r->nhist = TAPS; }
        r->hist[r->nhist][0] = in[i * ch];
        r->hist[r->nhist][1] = in[i * ch + (ch > 1)];
        r->nhist++;
        if (r->nhist < TAPS) continue;
        /* emit every output instant that falls before the next input frame */
        while (r->frac < (uint32_t)OUT_RATE && n < out_cap) {
            int p = (int)((uint64_t)r->frac * PHASES / OUT_RATE);
            const int16_t *c = r->coef[p];
            const int16_t (*h)[2] = r->hist + r->nhist - TAPS;
            int32_t L = 0, R = 0;
            for (int t = 0; t < TAPS; t++) { L += h[t][0] * c[t]; R += h[t][1] * c[t]; }
            out[2 * n] = clip(L >> 14);
            out[2 * n + 1] = clip(R >> 14);
            n++;
            r->frac += (uint32_t)r->in_rate;
        }
        r->frac -= OUT_RATE;
    }
    return n;
}
