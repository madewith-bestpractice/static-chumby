#pragma once
/* Bulletin slot list: what plays in each bulletin break, decided by the
 * bulletin board (GET https://bulletin.staticradio.app/api/slots/{date}), so
 * every listener on a station hears the same bulletin. The rule (same as the
 * phones, mobile_app/lib/audio/bulletin_slots.dart):
 *  - follow the list: a `paid` or `house` entry with content replaces the
 *    baked break's audio, link and cover;
 *  - no list, no entry, `baked`, an unknown kind: play the baked break;
 *  - never swap a break that is on the air (or about to be).
 * Breaks are matched on (station, placementId); the date is the US Central
 * broadcast date. Shared by the Chumby and Playdate ports (pure, no I/O). */
#include <stdint.h>
#include "json.h"
#include "sched.h"

#define SLOTS_URL "https://bulletin.staticradio.app/api/slots/"
#define SLOTS_LEAD_MS 120000        /* the one fetch before a break: inside the 7 min lock, so final */
#define SLOTS_MAX_LISTS 2

enum { SLOT_BAKED, SLOT_PAID, SLOT_HOUSE };

typedef struct {
    const char *cat;          /* app station id, e.g. nature_01 */
    const char *placement;
    int64_t at_ms;            /* play_at_utc */
    int kind;
    const char *bulletin_id, *audio, *link, *image;
} slot_t;

typedef struct {
    int n, cap;
    slot_t *v;
    int nt;
    char *text[SLOTS_MAX_LISTS];   /* owns the strings */
    jpool *pool[SLOTS_MAX_LISTS];
    char dates[SLOTS_MAX_LISTS][11];
} slotlist_t;

/* YYYY-MM-DD on the US Central calendar at utc_ms (US DST rule, no tz database). */
void slots_central_date(int64_t utc_ms, char out[11]);

/* Adds one served day (takes ownership of the malloc'd text). 0 on success. */
int slots_add(slotlist_t *l, char *text);
void slots_free(slotlist_t *l);

/* Applies the list to every station's bulletin breaks that start more than
 * 20 s after now (a break on the air, or about to be, keeps what it has).
 * Returns how many breaks changed. Replaced strings stay alive until the
 * schedule is freed (sched_keep). */
int slots_apply(sched_t *s, const slotlist_t *l, int64_t now);

/* Milliseconds until the next bulletin break on station st after the slot on
 * the air at now, or -1 if the station has none. */
int64_t slots_next_break_ms(const sched_t *s, int st, int64_t now);
