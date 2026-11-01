#include "common.h"
#include "slots.h"

/* Board channel id -> app station id (the worker's CHANNEL map). */
static const struct { int ch; const char *cat; } k_channels[] = {
    {1, "nature_01"}, {2, "work_01"}, {3, "learn_01"}, {4, "weird_01"},
    {5, "unite_01"}, {6, "make_01"}, {9, "news_01"},
};

#define MATCH_TOLERANCE_MS (2LL * 3600 * 1000)   /* nearest entry within 2 h is this break */
#define SWAP_GUARD_MS 20000                       /* never touch a break this close to air */

/* --- calendar (proleptic Gregorian, days since 1970-01-01) --------------- */

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* Day of month of the first Sunday of y-m (1970-01-01 was a Thursday). */
static int first_sunday(int y, int m) {
    int64_t d1 = days_from_civil(y, m, 1);
    int dow = (int)(((d1 % 7) + 7 + 4) % 7);   /* 0 = Sunday */
    return 1 + (7 - dow) % 7;
}

static int central_dst(int64_t utc_ms) {
    int y, m, d;
    civil_from_days(utc_ms / 86400000, &y, &m, &d);
    /* second Sunday of March 02:00 CST = 08:00 UTC; first Sunday of November 02:00 CDT = 07:00 UTC */
    int64_t start = (days_from_civil(y, 3, first_sunday(y, 3) + 7) * 86400 + 8 * 3600) * 1000;
    int64_t end = (days_from_civil(y, 11, first_sunday(y, 11)) * 86400 + 7 * 3600) * 1000;
    return utc_ms >= start && utc_ms < end;
}

void slots_central_date(int64_t utc_ms, char out[11]) {
    int64_t local = utc_ms - (central_dst(utc_ms) ? 5 : 6) * 3600000LL;
    int y, m, d;
    civil_from_days(local >= 0 ? local / 86400000 : (local - 86399999) / 86400000, &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

static int64_t iso_ms(const char *s) {
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    const char *z = s + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        t -= (*z == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return t * 1000;
}

/* --- the list ------------------------------------------------------------ */

static const char *nonempty(const jnode *o, const char *k) {
    const jnode *v = json_get(o, k);
    return v && v->t == J_STR && v->s[0] ? v->s : NULL;
}

int slots_add(slotlist_t *l, char *text) {
    if (l->nt >= SLOTS_MAX_LISTS) { free(text); return -1; }
    jpool *pool = NULL;
    jnode *root = json_parse(text, &pool);
    const char *date = root ? nonempty(root, "date") : NULL;
    const jnode *arr = root ? json_get(root, "slots") : NULL;
    if (!date || !arr || arr->t != J_ARR) { json_free(pool); free(text); return -1; }
    for (const jnode *e = arr->child; e; e = e->next) {
        const jnode *ch = json_get(e, "channel_id");
        const char *placement = nonempty(e, "placement_id");
        int64_t at = iso_ms(nonempty(e, "play_at_utc"));
        const char *cat = NULL;
        for (size_t i = 0; ch && ch->t == J_NUM && i < sizeof k_channels / sizeof k_channels[0]; i++)
            if (k_channels[i].ch == (int)ch->n) cat = k_channels[i].cat;
        if (!cat || !placement || !at) continue;          /* malformed: that break plays baked */
        const char *kind = nonempty(e, "kind");
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 64;
            l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
        }
        slot_t *s = &l->v[l->n++];
        s->cat = cat;
        s->placement = placement;
        s->at_ms = at;
        /* an unknown kind from a newer worker: baked, never guess */
        s->kind = kind && !strcmp(kind, "paid") ? SLOT_PAID : kind && !strcmp(kind, "house") ? SLOT_HOUSE : SLOT_BAKED;
        s->bulletin_id = nonempty(e, "bulletin_id");
        s->audio = nonempty(e, "audio_url");
        s->link = nonempty(e, "link_url");
    }
    snprintf(l->dates[l->nt], sizeof l->dates[0], "%s", date);
    l->text[l->nt] = text;
    l->pool[l->nt] = pool;
    l->nt++;
    return 0;
}

void slots_free(slotlist_t *l) {
    if (!l) return;
    for (int i = 0; i < l->nt; i++) { json_free(l->pool[i]); free(l->text[i]); }
    free(l->v);
    free(l);
}

/* The entry for this break, nearest to its start time, or NULL. */
static const slot_t *find(const slotlist_t *l, const char *cat, const char *placement, int64_t at) {
    const slot_t *best = NULL;
    int64_t best_d = MATCH_TOLERANCE_MS + 1;
    for (int i = 0; l && i < l->n; i++) {
        const slot_t *s = &l->v[i];
        if (strcmp(s->placement, placement) || strcmp(s->cat, cat)) continue;
        int64_t d = s->at_ms > at ? s->at_ms - at : at - s->at_ms;
        if (d < best_d) { best = s; best_d = d; }
    }
    return best;
}

int slots_apply(sched_t *s, const slotlist_t *l, int64_t now) {
    int changed = 0;
    for (int sti = 0; s && sti < s->n; sti++) {
        station_t *st = &s->st[sti];
        int cur;
        int64_t off;
        if (st->n == 0 || sched_live(s, sti, now, &cur, &off) != 0) continue;
        /* walk one loop ahead from the slot on the air */
        int64_t start = now - off;
        for (int k = 0; k < st->n; k++) {
            int i = (cur + k) % st->n;
            ep_t *ep = &st->eps[i];
            int64_t at = start;
            start += (int64_t)ep->dur_s * 1000 + GAP_MS;
            if (!ep->bulletin || !ep->placement || !ep->placement[0]) continue;
            if (k == 0 || at - now <= SWAP_GUARD_MS) continue;   /* on the air or about to be */
            const slot_t *sl = find(l, st->id, ep->placement, at);
            int content = sl && sl->kind != SLOT_BAKED && sl->bulletin_id && sl->audio;
            const char *want_url = content ? sl->audio : ep->b_url;
            const char *want_link = content ? (sl->link ? sl->link : ep->b_link) : ep->b_link;
            if (!strcmp(ep->url, want_url) && !strcmp(ep->link, want_link)) continue;
            if (want_url == ep->b_url) ep->url = ep->b_url;
            else { char *u = strdup(want_url); sched_keep(s, u); ep->url = u; }
            if (want_link == ep->b_link) ep->link = ep->b_link;
            else { char *u = strdup(want_link); sched_keep(s, u); ep->link = u; }
            LOG("slots: %s %s -> %s", st->id, ep->placement, content ? sl->bulletin_id : "baked");
            changed++;
        }
    }
    return changed;
}

int64_t slots_next_break_ms(const sched_t *s, int sti, int64_t now) {
    int cur;
    int64_t off;
    if (!s || sti < 0 || sti >= s->n || sched_live(s, sti, now, &cur, &off) != 0) return -1;
    const station_t *st = &s->st[sti];
    int64_t until = (int64_t)st->eps[cur].dur_s * 1000 + GAP_MS - off;
    for (int k = 1; k < st->n; k++) {
        const ep_t *ep = &st->eps[(cur + k) % st->n];
        if (ep->bulletin) return until;
        until += (int64_t)ep->dur_s * 1000 + GAP_MS;
    }
    return -1;
}
