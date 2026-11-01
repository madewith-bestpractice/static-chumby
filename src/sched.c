#include "common.h"
#include "sched.h"

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int hex_color(const char *h, uint16_t *out) {
    unsigned r, g, b;
    if (!h || h[0] != '#' || strlen(h) < 7 || sscanf(h + 1, "%02x%02x%02x", &r, &g, &b) != 3) return 0;
    *out = rgb565(r, g, b);
    return 1;
}

/* Station inks as of 2026-10 (palette.a), used until the published slim
 * schedule carries `palette` everywhere. */
static const struct { const char *id, *ink; } k_inks[] = {
    {"nature_01", "#4ADE80"}, {"work_01", "#FF5A36"}, {"learn_01", "#FFD34D"},
    {"news_01", "#38BDF8"}, {"weird_01", "#F1B8CC"}, {"unite_01", "#EF4444"},
    {"make_01", "#10B981"}, {"sheila_01", "#FF1493"}, {"sleep_01", "#818CF8"},
};

static uint16_t darken(uint16_t c, int num, int den) {
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (uint16_t)(((r * num / den) << 11) | ((g * num / den) << 5) | (b * num / den));
}

static int64_t parse_iso_ms(const char *s) {
    struct tm tm = {0};
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    int64_t t = (int64_t)timegm(&tm);
    /* offset suffix: Z or +HH:MM / -HH:MM after the seconds (and optional fraction) */
    const char *z = s + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        int sign = *z == '+' ? 1 : -1;
        t -= sign * (oh * 3600 + om * 60);
    }
    return t * 1000;
}

static int cmp_sort(const void *a, const void *b) {
    const int *x = a, *y = b;
    return x[0] - y[0];
}

/* Live streams from this device's own local-stations.json (see sched.h). */
static void add_local_stations(sched_t *s) {
    FILE *f = fopen(LOCAL_STATIONS_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = len > 0 && len < 65536 ? malloc((size_t)len + 1) : NULL;
    if (text && fread(text, 1, (size_t)len, f) != (size_t)len) { free(text); text = NULL; }
    fclose(f);
    if (!text) return;
    text[len] = 0;
    jnode *root = json_parse(text, &s->local_pool);
    if (!root || root->t != J_ARR) { LOG("sched: local-stations.json isn't a JSON array"); free(text); json_free(s->local_pool); s->local_pool = NULL; return; }
    s->local_text = text;
    for (const jnode *c = root->child; c && s->n < MAX_STATIONS; c = c->next) {
        const char *url = json_str(c, "url", NULL), *tag = json_str(c, "tag", NULL);
        if (!url || !*url || !tag || !*tag) continue;
        station_t st = {0};
        snprintf(st.id, sizeof st.id, "%s", json_str(c, "id", tag));
        snprintf(st.tag, sizeof st.tag, "%s", tag);
        st.ink = rgb565(0xF5, 0xF3, 0xEF);
        hex_color(json_str(c, "ink", NULL), &st.ink);
        st.bg = darken(st.ink, 1, 7);
        st.live = 1;
        st.eps = calloc(1, sizeof(ep_t));
        if (!st.eps) break;
        ep_t *ep = &st.eps[0];
        ep->url = url;
        ep->show = json_str(c, "name", tag);
        ep->title = json_str(c, "about", "");
        ep->feed = url;
        ep->image = json_str(c, "art", "");   /* an absolute path to 240x240 RGB565, or none */
        ep->link = json_str(c, "link", "");
        ep->dur_s = 86400 - 2;            /* one endless slot: the timeline maths never ends it */
        st.n = 1;
        st.loop_s = 86400;
        st.sum_ms = (int64_t)ep->dur_s * 1000;
        int at = s->n;                    /* default: the end of the dial */
        const char *after = json_str(c, "after", NULL);
        for (int i = 0; after && i < s->n; i++) if (!strcmp(s->st[i].id, after)) at = i + 1;
        memmove(&s->st[at + 1], &s->st[at], sizeof s->st[0] * (size_t)(s->n - at));
        s->st[at] = st;
        s->n++;
        LOG("sched: local live station %s at dial position %d", st.id, at);
    }
}

sched_t *sched_parse(char *text) {
    sched_t *s = calloc(1, sizeof *s);
    s->text = text;
    jnode *root = json_parse(text, &s->pool);
    if (!root) { LOG("sched: JSON parse failed"); sched_free(s); return NULL; }
    if (json_num(root, "schema_version", 0) < 7) { LOG("sched: schema too old"); sched_free(s); return NULL; }
    s->epoch_ms = parse_iso_ms(json_str(root, "daily_epoch_utc", NULL));
    snprintf(s->generated, sizeof s->generated, "%s", json_str(root, "generated_at_utc", ""));
    const jnode *cats = json_get(root, "categories");
    if (!s->epoch_ms || !cats || cats->t != J_OBJ) { LOG("sched: missing epoch/categories"); sched_free(s); return NULL; }

    /* active stations with episodes, ordered by sort_index */
    int order[MAX_STATIONS][2], n = 0;
    const jnode *cs[MAX_STATIONS];
    for (const jnode *c = cats->child; c && n < MAX_STATIONS; c = c->next) {
        if (!json_bool(c, "is_active", 1)) continue;
        const jnode *eps = json_get(c, "episodes");
        if (!eps || !eps->child) continue;
        cs[n] = c;
        order[n][0] = (int)json_num(c, "sort_index", 0);
        order[n][1] = n;
        n++;
    }
    qsort(order, (size_t)n, sizeof order[0], cmp_sort);

    for (int i = 0; i < n; i++) {
        const jnode *c = cs[order[i][1]];
        station_t *st = &s->st[s->n];
        snprintf(st->id, sizeof st->id, "%s", json_str(c, "id", c->key ? c->key : "?"));
        const char *tag = json_str(c, "u_tag", NULL);
        if (tag && *tag) snprintf(st->tag, sizeof st->tag, "%s", tag);
        else {
            snprintf(st->tag, sizeof st->tag, "%s", json_str(c, "display_name", st->id));
            for (char *p = st->tag; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        size_t tl = strlen(st->tag);   /* "SHEILA B" -> "SHEILA" (the watches do the same) */
        if (tl > 2 && st->tag[tl - 2] == ' ' && (st->tag[tl - 1] == 'A' || st->tag[tl - 1] == 'B')) st->tag[tl - 2] = 0;
        if (!strcmp(st->tag, "WEIRD")) strcpy(st->tag, "WEiRD");   /* the phone's stylized lowercase i (main.dart) */

        const jnode *pal = json_get(c, "palette");
        st->ink = rgb565(0xF5, 0xF3, 0xEF);
        int have_ink = hex_color(json_str(pal, "a", NULL), &st->ink);
        for (size_t k = 0; !have_ink && k < sizeof k_inks / sizeof k_inks[0]; k++)
            if (!strcmp(k_inks[k].id, st->id)) have_ink = hex_color(k_inks[k].ink, &st->ink);
        if (!hex_color(json_str(pal, "bg", NULL), &st->bg)) st->bg = darken(st->ink, 1, 7);
        /* SHEILA's bg is a light pink: keep the dial readable with a dark bg */
        if ((((st->bg >> 11) & 31) + ((st->bg >> 5) & 63) / 2 + (st->bg & 31)) > 60) st->bg = darken(st->ink, 1, 7);

        st->loop_s = (int64_t)json_num(c, "loop_duration_seconds", 0);
        const jnode *eps = json_get(c, "episodes");
        int cnt = 0;
        for (const jnode *e = eps->child; e; e = e->next) cnt++;
        st->eps = calloc((size_t)cnt, sizeof(ep_t));
        for (const jnode *e = eps->child; e; e = e->next) {
            int dur = (int)json_num(e, "durationInSeconds", 0);
            const char *url = json_str(e, "audioUrl", NULL);
            if (dur <= 0) continue;   /* the phone skips zero-length slots entirely */
            ep_t *ep = &st->eps[st->n++];
            ep->url = url ? url : "";
            ep->title = json_str(e, "title", "");
            ep->show = json_str(e, "feedTitle", "");
            ep->feed = json_str(e, "feedUrl", "");
            ep->image = json_str(e, "imageUrl", "");
            ep->link = json_str(e, "episodeUrl", "");
            ep->dur_s = dur;
            ep->gain_db = json_num(e, "gainDb", json_num(e, "volume_offset_db", 0));
            const char *sponsor = json_str(e, "sponsorName", "");
            ep->bulletin = json_bool(e, "isBulletin", 0) || !strcasecmp(sponsor, "community bulletin");
            ep->placement = json_str(e, "placementId", NULL);
            ep->b_url = ep->url;
            ep->b_link = ep->link;
            st->sum_ms += (int64_t)dur * 1000;
        }
        if (st->n == 0 || st->loop_s <= 0) { free(st->eps); continue; }
        s->n++;
    }
    if (s->n == 0) { LOG("sched: no playable stations"); sched_free(s); return NULL; }
    add_local_stations(s);
    return s;
}

void sched_keep(sched_t *s, char *str) {
    if (s->nkeep == s->ckeep) {
        s->ckeep = s->ckeep ? s->ckeep * 2 : 16;
        s->keep = realloc(s->keep, (size_t)s->ckeep * sizeof *s->keep);
    }
    s->keep[s->nkeep++] = str;
}

void sched_free(sched_t *s) {
    if (!s) return;
    for (int i = 0; i < s->nkeep; i++) free(s->keep[i]);
    free(s->keep);
    for (int i = 0; i < s->n; i++) free(s->st[i].eps);
    json_free(s->pool);
    free(s->text);
    json_free(s->local_pool);
    free(s->local_text);
    free(s);
}

static int locate(const sched_t *s, int sti, int64_t t_ms, int *slot, int64_t *off_ms, int64_t *slot_ms) {
    if (!s || sti < 0 || sti >= s->n) return -1;
    const station_t *st = &s->st[sti];
    int64_t pad = (int64_t)st->n * GAP_MS;
    int64_t loop = st->loop_s * 1000 + pad;
    int64_t total = st->sum_ms + pad;
    int64_t pos = (t_ms - s->epoch_ms) % loop;
    if (pos < 0) pos += loop;
    pos %= total;
    int64_t acc = 0;
    for (int i = 0; i < st->n; i++) {
        int64_t len = (int64_t)st->eps[i].dur_s * 1000 + GAP_MS;
        if (acc + len > pos) { *slot = i; *off_ms = pos - acc; if (slot_ms) *slot_ms = len; return 0; }
        acc += len;
    }
    return -1;
}

int sched_live(const sched_t *s, int st, int64_t t_ms, int *slot, int64_t *off_ms) {
    return locate(s, st, t_ms, slot, off_ms, NULL);
}

int64_t sched_slot_remaining(const sched_t *s, int st, int64_t t_ms) {
    int slot; int64_t off, len;
    if (locate(s, st, t_ms, &slot, &off, &len)) return 0;
    return len - off;
}
