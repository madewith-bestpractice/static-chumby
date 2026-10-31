#pragma once
#include <stdint.h>

/* Fixed-point polyphase resampler to 44.1 kHz stereo s16 (the output the
 * audio engine plays). 16 taps x 128 phases, windowed sinc; integer-only per
 * sample (no FPU on this CPU). Mono input is duplicated to both channels. */
typedef struct rs rs_t;

rs_t *rs_new(int in_rate);
void rs_free(rs_t *r);
/* Push `frames` interleaved frames with `ch` (1 or 2) channels; writes up to
 * out_cap stereo frames to out. Returns frames written. */
int rs_push(rs_t *r, const int16_t *in, int frames, int ch, int16_t *out, int out_cap);
