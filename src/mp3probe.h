#pragma once
#include <stddef.h>
#include <stdint.h>

/* What we need to open an MP3 at a time offset with ONE ranged request:
 * where audio starts (after ID3v2), the bitrate, and for VBR files the Xing
 * seek table. */
typedef struct {
    int ok;
    int64_t audio_start;   /* first byte after the ID3v2 tag(s) */
    int64_t total;         /* file size (from Content-Range/Length), 0 if unknown */
    int bitrate_kbps;      /* of the first frame (CBR) */
    int srate;
    int has_toc;           /* Xing VBR table present */
    int64_t frames;        /* Xing frame count, 0 if unknown */
    int64_t xing_bytes;    /* Xing byte count, 0 if unknown */
    int samples_per_frame;
    uint8_t toc[100];
} mp3info_t;

/* Size of the ID3v2 tag at the start of buf (0 if none); needs >= 10 bytes. */
int64_t mp3_id3_size(const uint8_t *buf, size_t len);

/* Parse the first frame (and any Xing/Info header) at the start of buf.
 * Fills info (audio_start/total must already be set). Returns 1 on success. */
int mp3_parse_first_frame(const uint8_t *buf, size_t len, mp3info_t *info);

/* Byte offset to start reading for `ms` into the audio (frame-aligned by the
 * decoder's resync). */
int64_t mp3_byte_for_ms(const mp3info_t *info, int64_t ms);
