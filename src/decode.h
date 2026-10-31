#pragma once
#include <stdint.h>
#include "fetch.h"
#include "mp3probe.h"

/* In-process decoding (replaces the ffmpeg process for known formats):
 * fetch (or a neighbor pre-buffer) -> Helix MP3/AAC or WAV -> resample to
 * 44.1 kHz stereo s16 -> out_fd (the pipe the audio engine reads). */
enum { FMT_UNKNOWN, FMT_MP3, FMT_MP4, FMT_WAV };

typedef struct {
    int fmt;
    mp3info_t mp3;            /* FMT_MP3 */
    char moov_path[256];      /* FMT_MP4: the audio track's moov, cached on disk */
    int64_t mp4_first, mp4_end, mp4_dur_ms;   /* audio data span + duration */
    uint32_t mp4_toc[257];    /* byte at each 1/256 of the duration (VBR-safe estimate) */
    int64_t wav_data;         /* FMT_WAV: data chunk offset */
    int wav_rate, wav_ch;
} probe_t;

/* Start a decoder thread for `url` (already resolved) at `seek_ms`. If `pb` is
 * non-NULL and covers the start byte, it feeds the decoder (no new request);
 * the decoder then owns it. Writes PCM to out_fd and closes it at the end.
 * Returns 0 if the thread started (it owns out_fd either way on success). */
int decode_start(const char *url, const probe_t *probe, int64_t seek_ms, prebuf_t *pb, int out_fd, int *used_pb);
