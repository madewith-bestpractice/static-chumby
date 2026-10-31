#pragma once
#include <stddef.h>
#include <stdint.h>

/* The audio track's index from an MP4/M4A `moov` box: enough to start AAC
 * playback at any time with one ranged request (we feed raw AAC frames to the
 * decoder ourselves; ffmpeg's index parse + extra requests took 15-20 s). */
typedef struct {
    uint32_t timescale;
    uint32_t nsamples;
    uint32_t n_stts, *stts_count, *stts_delta;
    uint32_t uniform_size, *sizes;           /* stsz */
    uint32_t n_stsc, *stsc_first, *stsc_spc;  /* stsc (first chunk is 1-based) */
    uint32_t n_chunks;
    uint64_t *chunk_off;                      /* stco/co64 */
    uint8_t asc[64];
    int asc_len;
    int aot, channels, core_rate;             /* from the AudioSpecificConfig */
} mp4idx_t;

int mp4_parse_moov(const uint8_t *moov, size_t len, mp4idx_t *out);   /* 0 on success */
void mp4_free(mp4idx_t *m);

/* First sample at or after `ms`; its exact time in *sample_ms. */
uint32_t mp4_sample_for_ms(const mp4idx_t *m, int64_t ms, int64_t *sample_ms);

typedef struct { uint32_t k, chunk, in_chunk, stsc_i; uint64_t off; } mp4iter_t;
int mp4_iter_seek(const mp4idx_t *m, uint32_t k, mp4iter_t *it);
/* The current sample's byte offset and size; advances. 0 at the end. */
int mp4_iter_next(const mp4idx_t *m, mp4iter_t *it, uint64_t *off, uint32_t *size);
