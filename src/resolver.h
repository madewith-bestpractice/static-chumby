#pragma once
#include <stddef.h>
#include "sched.h"

/* Background thread: fetches the schedule every 5 minutes and walks podcast
 * redirect chains AHEAD of time (each TLS handshake costs ~2.4 s on this CPU,
 * a 4-hop tracker chain ~15 s), so tuning needs one handshake. */
int resolver_start(void);

/* A newer schedule, or NULL. Caller owns it. */
sched_t *resolver_take_schedule(void);

/* Priority-ordered URLs to keep resolved (main thread replaces the list). */
void resolver_want(const char *const *urls, int n);

/* Final URL if cached and fresh; returns 1 and copies into out. */
int resolver_lookup(const char *url, char *out, size_t cap);
/* 0 not resolved yet, 1 resolved + playable natively, 2 resolved (ffmpeg
 * only), 3 resolution failed. */
int resolver_status(const char *url);
/* Like resolver_lookup, but only for MP3s we probed (ID3 size, bitrate, Xing
 * table): lets the player open at a computed byte offset in one request. */
#include "mp3probe.h"
int resolver_lookup_mp3(const char *url, char *out, size_t cap, mp3info_t *info);
/* Any format we can play natively (MP3, MP4/AAC with a cached moov, WAV). */
#include "decode.h"
int resolver_lookup_probe(const char *url, char *out, size_t cap, probe_t *info);
void resolver_invalidate(const char *url);

/* Pre-baked riso art (art/riso/v1/chumby240/{sha1(imageUrl)}.rgb565, 240x240
 * RGB565): main says which covers it wants (first = most urgent); returns the
 * pixels if cached, else NULL. Pointers stay valid until the next want call. */
void resolver_want_art(const char *const *image_urls, int n);
const uint16_t *resolver_art(const char *image_url);

/* Announce presence on this station id ("" = none), phone-compatible. */
void resolver_set_presence(const char *station);
/* Live listeners on a station from staticradio.app/api/counts; -1 unknown. */
int resolver_count(const char *station);
/* 1 once a live schedule has been fetched this run. */
int resolver_network_ready(void);
