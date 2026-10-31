#pragma once
#include <stdint.h>
#include "json.h"

typedef struct {
    const char *url, *title, *show;
    const char *feed;    /* feedUrl: the fave key (phone parity: feedUrl ?? audioUrl) */
    const char *image;   /* imageUrl: names the pre-baked riso art (sha1 of this string) */
    const char *link;    /* episodeUrl: the episode's web page (info-view QR code) */
    int dur_s;
    double gain_db;
    int bulletin;
} ep_t;

typedef struct {
    char id[24];
    char tag[24];        /* dial label, e.g. NEWS */
    uint16_t ink, bg;    /* RGB565 */
    int64_t loop_s;      /* manifest loop_duration_seconds */
    int64_t sum_ms;      /* sum of episode durations */
    int n;
    ep_t *eps;           /* only episodes with dur > 0, in loop order */
} station_t;

#define MAX_STATIONS 16

typedef struct {
    int64_t epoch_ms;
    char generated[48];
    int n;
    station_t st[MAX_STATIONS];
    char *text;          /* owns all strings */
    jpool *pool;
} sched_t;

/* Takes ownership of `text` (malloc'd). NULL on failure. */
sched_t *sched_parse(char *text);
void sched_free(sched_t *s);

/* The phone's wall-clock -> (slot, offset) rule, with the 1.5 s gap after every
 * episode (sync_controller.dart _resolveLiveEpisodeForSchedule). */
int sched_live(const sched_t *s, int st, int64_t t_ms, int *slot, int64_t *off_ms);

/* Milliseconds from t until the slot containing t ends (audio + trailing gap). */
int64_t sched_slot_remaining(const sched_t *s, int st, int64_t t_ms);
