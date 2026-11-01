#include "common.h"
#include "resolver.h"
#include "net.h"
#include "json.h"
#include "mp3probe.h"
#include "decode.h"
#include "mp4.h"
#include <mbedtls/sha1.h>
#include <sys/stat.h>

/* Disk caches so a restart shows the dial, the art and starts tuning before
 * Wi-Fi is even up (the network refreshes them in the background). */
#define SCHEDULE_CACHE STATIC_DIR "/schedule-cache.json"
#define ART_DIR STATIC_DIR "/art"

static int write_file(const char *path, const void *data, size_t len) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok ? rename(tmp, path) : -1;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}
#include <pthread.h>
#include <unistd.h>

#define CACHE_N 64
#define WANT_N 32
#define FRESH_MS (20 * 60 * 1000)        /* signed CDN URLs: re-resolve after 20 min */
#define SCHEDULE_EVERY_MS (5 * 60 * 1000) /* phone parity */

typedef struct { char *orig, *final; int64_t at; int failed; probe_t probe; } entry_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static entry_t cache[CACHE_N];
static char *want[WANT_N];
static char *active;   /* the URL being resolved right now (under mu) */
static int nwant;
static sched_t *pending;
static char last_generated[48];
static int have_live_schedule;           /* fetched over the network this run */

static void load_cached_schedule(void) {
    char *text = read_file(SCHEDULE_CACHE, NULL);
    if (!text) return;
    sched_t *s = sched_parse(text);
    if (!s) return;
    /* listener sync: never play a previous broadcast day's timeline */
    int64_t age = now_ms() - s->epoch_ms;
    if (age > 30LL * 3600 * 1000 || age < -30LL * 3600 * 1000) { LOG("schedule: cache is another day; waiting for the network"); sched_free(s); return; }
    snprintf(last_generated, sizeof last_generated, "%s", s->generated);
    LOG("schedule: cached copy, generated %s", s->generated);
    pthread_mutex_lock(&mu);
    pending = s;
    pthread_mutex_unlock(&mu);
}

/* Presence + listener counts: the phone's contract (presence_manager.dart).
 * PUT {rtdb}/presence/{station}/{device}.json {"ts":{".sv":"timestamp"}} every
 * 30 s (live for 75 s); DELETE when leaving. Counts: GET staticradio.app/api/counts. */
#define RTDB "https://static-3c8b6-default-rtdb.firebaseio.com"
#define COUNTS_URL "https://staticradio.app/api/counts"
#define HEARTBEAT_MS 30000
static char presence_want[24], presence_on[24], device_id[24];
static struct { char id[24]; int n; } counts[MAX_STATIONS];
static int ncounts;

/* riso art cache: a few full-screen frames (150 KB each) */
#define ART_N 12   /* every station's live cover (+ a fave), 115 KB each */
#define ART_BYTES (240 * 240 * 2)
#define ART_BASE "https://data.staticradio.app/art/riso/v1/chumby240/"
typedef struct { char *url; uint16_t *px; int64_t at; int missing; } art_t;
static art_t art[ART_N];
static char *art_want[ART_N];
static int nart_want;

static art_t *art_find(const char *url) {
    for (int i = 0; i < ART_N; i++) if (art[i].url && !strcmp(art[i].url, url)) return &art[i];
    return NULL;
}

void resolver_want_art(const char *const *urls, int n) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nart_want; i++) free(art_want[i]);
    nart_want = 0;
    for (int i = 0; i < n && nart_want < ART_N; i++)
        if (urls[i] && *urls[i] && urls[i][0] != '/') art_want[nart_want++] = strdup(urls[i]);   /* '/': a local file */
    pthread_mutex_unlock(&mu);
}

/* Art from a local file (local-stations.json "art": an absolute path to 240x240
 * RGB565, little-endian, like the baked covers). Main thread only. */
static const uint16_t *local_art(const char *path) {
    static struct { char path[256]; uint16_t *px; int missing; } c[4];
    static int next;
    for (int i = 0; i < 4; i++) if (!strcmp(c[i].path, path)) return c[i].missing ? NULL : c[i].px;
    int i = next++ % 4;
    free(c[i].px); c[i].px = NULL;
    snprintf(c[i].path, sizeof c[i].path, "%s", path);
    size_t len = 0;
    char *b = read_file(path, &len);
    if (b && len == ART_BYTES) c[i].px = (uint16_t *)b;
    else { free(b); LOG("art: %s isn't a 240x240 RGB565 file", path); }
    c[i].missing = !c[i].px;
    return c[i].px;
}

const uint16_t *resolver_art(const char *url) {
    if (url && url[0] == '/') return local_art(url);
    pthread_mutex_lock(&mu);
    art_t *a = url && *url ? art_find(url) : NULL;
    const uint16_t *px = a && !a->missing ? a->px : NULL;
    pthread_mutex_unlock(&mu);
    return px;
}

/* Fetch one wanted cover that isn't cached; evicts the least wanted. */
static int fetch_one_art(net_ctx_t *net) {
    char *url = NULL;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nart_want && !url; i++) {
        art_t *a = art_find(art_want[i]);
        if (!a || (a->missing && now_ms() - a->at > 2 * 60 * 1000)) url = strdup(art_want[i]);
    }
    pthread_mutex_unlock(&mu);
    if (!url) return 0;
    unsigned char h[20];
    mbedtls_sha1((const unsigned char *)url, strlen(url), h);
    char full[160];
    int o = snprintf(full, sizeof full, "%s", ART_BASE);
    for (int i = 0; i < 20; i++) o += snprintf(full + o, sizeof full - (size_t)o, "%02x", h[i]);
    snprintf(full + o, sizeof full - (size_t)o, ".rgb565");
    char disk[160];
    snprintf(disk, sizeof disk, ART_DIR "/%s", full + strlen(ART_BASE));
    buf_t b = {0};
    int st = 0;
    size_t dlen = 0;
    b.data = read_file(disk, &dlen);
    if (b.data && dlen == ART_BYTES) { b.len = dlen; st = 200; }
    else {
        free(b.data); b.data = NULL;
        st = http_send(net, "GET", full, NULL, &b);
        if (st == 200 && b.len == ART_BYTES) { mkdir(ART_DIR, 0755); write_file(disk, b.data, b.len); }
    }
    int ok = st == 200 && b.len == ART_BYTES;
    pthread_mutex_lock(&mu);
    art_t *slot = art_find(url);
    if (!slot) {   /* evict an entry nobody wants any more, else the oldest */
        for (int i = 0; i < ART_N && !slot; i++) {
            int wanted = 0;
            for (int j = 0; j < nart_want; j++) if (art[i].url && !strcmp(art[i].url, art_want[j])) wanted = 1;
            if (!art[i].url || !wanted) slot = &art[i];
        }
        if (!slot) slot = &art[0];
        free(slot->url); free(slot->px);
        slot->url = strdup(url); slot->px = NULL;
    }
    slot->at = now_ms();
    slot->missing = !ok;
    if (ok) { free(slot->px); slot->px = (uint16_t *)b.data; b.data = NULL; }
    pthread_mutex_unlock(&mu);
    if (!ok) LOG("art: %d (%zu bytes) for %.60s", st, b.len, url);
    buf_free(&b);
    free(url);
    return 1;
}

static int direct_host(const char *u) {
    /* our own hosts serve the file directly; no chain to walk */
    return strstr(u, "://data.staticradio.app/") || strstr(u, "://bulletin.staticradio.app/");
}

static entry_t *find(const char *url) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].orig && !strcmp(cache[i].orig, url)) return &cache[i];
    return NULL;
}

int resolver_lookup_probe(const char *url, char *out, size_t cap, probe_t *info) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    entry_t *e = find(url);
    if (e && e->final && e->probe.fmt != FMT_UNKNOWN && now_ms() - e->at < FRESH_MS) {
        snprintf(out, cap, "%s", e->final);
        *info = e->probe;
        ok = 1;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

int resolver_lookup_mp3(const char *url, char *out, size_t cap, mp3info_t *info) {
    probe_t p;
    if (!resolver_lookup_probe(url, out, cap, &p) || p.fmt != FMT_MP3) return 0;
    *info = p.mp3;
    return 1;
}

int resolver_status(const char *url) {
    int st = 0;
    pthread_mutex_lock(&mu);
    entry_t *e = find(url);
    if (e && e->failed) st = 3;
    else if (e && e->final && now_ms() - e->at < FRESH_MS) st = e->probe.fmt != FMT_UNKNOWN ? 1 : 2;
    else if (active && !strcmp(active, url)) st = 4;   /* being resolved right now */
    pthread_mutex_unlock(&mu);
    return st;
}

int resolver_lookup(const char *url, char *out, size_t cap) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    entry_t *e = find(url);
    if (e && e->final && now_ms() - e->at < FRESH_MS) { snprintf(out, cap, "%s", e->final); ok = 1; }
    pthread_mutex_unlock(&mu);
    return ok;
}

void resolver_invalidate(const char *url) {
    pthread_mutex_lock(&mu);
    entry_t *e = find(url);
    if (e) { free(e->final); e->final = NULL; e->at = 0; }
    pthread_mutex_unlock(&mu);
}

void resolver_want(const char *const *urls, int n) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nwant; i++) free(want[i]);
    nwant = 0;
    for (int i = 0; i < n && nwant < WANT_N; i++)
        if (urls[i] && *urls[i] && !direct_host(urls[i])) want[nwant++] = strdup(urls[i]);
    pthread_mutex_unlock(&mu);
}

static slotlist_t *slots_pending;
static int64_t slots_at;          /* next slot-list fetch (0 = now) */
#define SLOTS_EVERY_MS (10 * 60 * 1000)

slotlist_t *resolver_take_slots(void) {
    pthread_mutex_lock(&mu);
    slotlist_t *l = slots_pending;
    slots_pending = NULL;
    pthread_mutex_unlock(&mu);
    return l;
}

void resolver_slots_request(void) {
    pthread_mutex_lock(&mu);
    slots_at = 0;
    pthread_mutex_unlock(&mu);
}

/* Today's list on the Central calendar, plus tomorrow's when a break within
 * the next 3 h falls after Central midnight. 1 if a list came back. */
static int fetch_slots(net_ctx_t *net) {
    char dates[2][11];
    int64_t now = now_ms();
    slots_central_date(now, dates[0]);
    slots_central_date(now + 3 * 3600 * 1000LL, dates[1]);
    int nd = strcmp(dates[0], dates[1]) ? 2 : 1;
    slotlist_t *l = calloc(1, sizeof *l);
    for (int i = 0; i < nd; i++) {
        char url[128];
        snprintf(url, sizeof url, SLOTS_URL "%s", dates[i]);
        buf_t b = {0};
        int st = http_get(net, url, 1 << 20, &b);
        if (st == 200 && b.data) { if (slots_add(l, b.data)) LOG("slots: bad list for %s", dates[i]); }
        else { LOG("slots: fetch %s failed (%d)", dates[i], st); buf_free(&b); }
    }
    if (!l->nt) { slots_free(l); return 0; }
    pthread_mutex_lock(&mu);
    if (slots_pending) slots_free(slots_pending);
    slots_pending = l;
    pthread_mutex_unlock(&mu);
    return 1;
}

sched_t *resolver_take_schedule(void) {
    pthread_mutex_lock(&mu);
    sched_t *s = pending;
    pending = NULL;
    pthread_mutex_unlock(&mu);
    return s;
}

static void store(const char *orig, const char *final, const probe_t *probe) {
    pthread_mutex_lock(&mu);
    entry_t *e = find(orig);
    if (!e) {   /* evict the oldest */
        e = &cache[0];
        for (int i = 1; i < CACHE_N; i++) if (cache[i].at < e->at) e = &cache[i];
        free(e->orig); free(e->final);
        e->orig = strdup(orig); e->final = NULL;
    }
    free(e->final);
    e->final = final ? strdup(final) : NULL;
    e->failed = final == NULL;
    if (probe) e->probe = *probe; else memset(&e->probe, 0, sizeof e->probe);
    e->at = now_ms();
    pthread_mutex_unlock(&mu);
}

/* ---------- probing: what is this file, and how do we start it mid-way? ---------- */

#define MOOV_DIR STATIC_DIR "/moov"
#define MOOV_MAX (16 * 1024 * 1024)

/* sequential reader over ranged requests */
typedef struct { net_ctx_t *net; const char *url; net_stream_t *s; int64_t pos, total; } rd_t;

static int rd_open(rd_t *r, int64_t at) {
    http_stream_close(r->s);
    int st; int64_t total;
    r->s = http_stream_open(r->net, r->url, at, -1, &st, &total);
    if (!r->s || (st != 206 && !(st == 200 && at == 0))) { http_stream_close(r->s); r->s = NULL; return -1; }
    if (total) r->total = total;
    r->pos = at;
    return 0;
}

static int rd_read(rd_t *r, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int x = http_stream_read(r->s, (char *)buf + got, n - got);
        if (x <= 0) return -1;
        got += (size_t)x;
    }
    r->pos += (int64_t)n;
    return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void moov_path_for(const char *orig, char *out, size_t cap) {
    unsigned char h[20];
    mbedtls_sha1((const unsigned char *)orig, strlen(orig), h);
    int o = snprintf(out, cap, MOOV_DIR "/");
    for (int i = 0; i < 20; i++) o += snprintf(out + o, cap - (size_t)o, "%02x", h[i]);
    snprintf(out + o, cap - (size_t)o, ".moov");
}

/* Parse a cached moov; fills the data span + duration used to estimate bytes. */
static int moov_ok(const char *path, probe_t *pr) {
    size_t len = 0;
    char *b = read_file(path, &len);
    if (!b) return 0;
    mp4idx_t m;
    int ok = mp4_parse_moov((uint8_t *)b, len, &m) == 0;
    if (ok) {
        uint64_t dur = 0;
        for (uint32_t i = 0; i < m.n_stts; i++) dur += (uint64_t)m.stts_count[i] * m.stts_delta[i];
        mp4iter_t it;
        uint64_t off = 0; uint32_t sz = 0;
        pr->mp4_first = (int64_t)m.chunk_off[0];
        if (!mp4_iter_seek(&m, m.nsamples - 1, &it) && mp4_iter_next(&m, &it, &off, &sz)) pr->mp4_end = (int64_t)(off + sz);
        pr->mp4_dur_ms = (int64_t)(dur * 1000 / m.timescale);
        /* seek table: the exact byte at each 1/256 of the duration (the bitrate
         * varies over a 3 h file; a straight line was ~100 s off) */
        for (int i = 0; i <= 256; i++) {
            uint32_t k = mp4_sample_for_ms(&m, pr->mp4_dur_ms * i / 256, NULL);
            pr->mp4_toc[i] = !mp4_iter_seek(&m, k, &it) ? (uint32_t)it.off : (uint32_t)pr->mp4_end;
        }
        mp4_free(&m);
    }
    free(b);
    return ok;
}

/* A long probe (a multi-MB moov takes 15 s+) gives way when the tuned
 * station's episode needs resolving: it's abandoned, not marked failed, and
 * comes round again later. Resolver thread only. */
static int probe_yielded;
static int yield_to_tuned(void) {
    int y = 0;
    pthread_mutex_lock(&mu);
    if (nwant && active && strcmp(want[0], active)) {
        entry_t *e = find(want[0]);
        y = !e || (e->failed && now_ms() - e->at > 5000);
    }
    pthread_mutex_unlock(&mu);
    return y;
}

/* Walk the top-level boxes to the moov (at the start, or after mdat) and cache it. */
static int probe_mp4(net_ctx_t *net, const char *final, const char *path) {
    rd_t r = { net, final, NULL, 0, 0 };
    if (rd_open(&r, 0)) return 0;
    int ok = 0;
    for (int n = 0; n < 16; n++) {
        uint8_t hdr[16];
        if (rd_read(&r, hdr, 8)) break;
        int64_t start = r.pos - 8, size = be32(hdr);
        uint32_t type = be32(hdr + 4);
        if (size == 1) { if (rd_read(&r, hdr + 8, 8)) break; size = (int64_t)((uint64_t)be32(hdr + 8) << 32 | be32(hdr + 12)); }
        else if (size == 0) size = r.total - start;
        if (size < 8) break;
        if (type == 0x6d6f6f76 /* moov */) {
            if (size > MOOV_MAX) break;
            uint8_t *m = malloc((size_t)size);
            if (!m) break;
            size_t hl = (size_t)(r.pos - start);
            memcpy(m, hdr, hl);
            int got_all = 1;
            for (size_t at = hl; at < (size_t)size; ) {   /* in 64 KB steps, so it can give way */
                size_t k = (size_t)size - at > 65536 ? 65536 : (size_t)size - at;
                if (rd_read(&r, m + at, k)) { got_all = 0; break; }
                at += k;
                if (at < (size_t)size && yield_to_tuned()) { probe_yielded = 1; got_all = 0; break; }
            }
            if (got_all) {
                mp4idx_t idx;
                if (mp4_parse_moov(m, (size_t)size, &idx) == 0) {   /* an AAC track we can play */
                    mp4_free(&idx);
                    mkdir(MOOV_DIR, 0755);
                    ok = write_file(path, m, (size_t)size) == 0;
                }
            }
            free(m);
            break;
        }
        int64_t next = start + size;
        if (r.total && next >= r.total) break;
        if (size - (r.pos - start) > 256 * 1024) { if (rd_open(&r, next)) break; }   /* jump (mdat) */
        else {                                                                     /* read through */
            char skip[4096];
            int64_t left = next - r.pos;
            while (left > 0) { size_t k = left > (int64_t)sizeof skip ? sizeof skip : (size_t)left; if (rd_read(&r, skip, k)) goto out; left -= (int64_t)k; }
        }
    }
out:
    http_stream_close(r.s);
    return ok;
}

/* url: the resolved URL (a 2048-byte buffer); updated if the read redirected. */
static void probe_any(net_ctx_t *net, const char *orig, char *url, probe_t *pr) {
    memset(pr, 0, sizeof *pr);
    char path[256];
    moov_path_for(orig, path, sizeof path);
    if (moov_ok(path, pr)) { pr->fmt = FMT_MP4; snprintf(pr->moov_path, sizeof pr->moov_path, "%s", path); return; }
    mp3info_t *info = &pr->mp3;
    int st; int64_t total;
    net_stream_t *s = http_stream_open(net, url, 0, -1, &st, &total);
    if (!s) return;
    if (strcmp(http_stream_url(s), url)) {   /* the real read bounced: remember where it lives */
        LOG("resolve: reads redirect to %.80s", http_stream_url(s));
        snprintf(url, 2048, "%s", http_stream_url(s));
    }
    static uint8_t buf[16384];
    size_t len = 0;
    int r;
    while (len < 1024 && (r = http_stream_read(s, (char *)buf + len, sizeof buf - len)) > 0) len += (size_t)r;
    if ((st != 200 && st != 206) || len < 44) { http_stream_close(s); return; }
    if (!memcmp(buf + 4, "ftyp", 4)) {                                   /* MP4/M4A */
        http_stream_close(s);
        if (probe_mp4(net, url, path) && moov_ok(path, pr)) { pr->fmt = FMT_MP4; snprintf(pr->moov_path, sizeof pr->moov_path, "%s", path); }
        return;
    }
    if (!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {      /* WAV: find fmt + data */
        http_stream_close(s);
        for (size_t p = 12; p + 8 <= len; ) {
            uint32_t sz = (uint32_t)buf[p + 4] | buf[p + 5] << 8 | buf[p + 6] << 16 | (uint32_t)buf[p + 7] << 24;
            if (!memcmp(buf + p, "fmt ", 4) && p + 24 <= len) {
                int fmtc = buf[p + 8] | buf[p + 9] << 8, bits = buf[p + 22] | buf[p + 23] << 8;
                pr->wav_ch = buf[p + 10] | buf[p + 11] << 8;
                pr->wav_rate = (int)((uint32_t)buf[p + 12] | buf[p + 13] << 8 | buf[p + 14] << 16 | (uint32_t)buf[p + 15] << 24);
                if (fmtc != 1 || bits != 16 || pr->wav_ch < 1 || pr->wav_ch > 2) return;
            } else if (!memcmp(buf + p, "data", 4)) {
                if (pr->wav_rate) { pr->wav_data = (int64_t)p + 8; pr->fmt = FMT_WAV; }
                return;
            }
            p += 8 + sz + (sz & 1);
        }
        return;
    }
    int64_t id3 = mp3_id3_size(buf, len);
    if (id3 > 4 * 1024 * 1024) { http_stream_close(s); return; }
    /* skip to the end of the tag (embedded cover art can make it large) */
    int64_t pos = (int64_t)len;
    size_t keep = 0;
    if (id3 < pos) { keep = (size_t)(pos - id3); memmove(buf, buf + id3, keep); }
    while (pos < id3) {
        r = http_stream_read(s, (char *)buf, sizeof buf);
        if (r <= 0) { http_stream_close(s); return; }
        if (pos + r > id3) { keep = (size_t)(pos + r - id3); memmove(buf, buf + (id3 - pos), keep); }
        pos += r;
    }
    while (keep < sizeof buf && (r = http_stream_read(s, (char *)buf + keep, sizeof buf - keep)) > 0) keep += (size_t)r;
    http_stream_close(s);
    info->audio_start = id3;
    info->total = total;
    if (mp3_parse_first_frame(buf, keep, info)) pr->fmt = FMT_MP3;
}

static void fetch_schedule(net_ctx_t *net) {
    buf_t b = {0};
    int64_t t0 = now_ms();
    int st = http_get(net, SCHEDULE_URL, 4 << 20, &b);
    if (st != 200 || !b.data) { LOG("schedule: fetch failed (%d)", st); buf_free(&b); return; }
    char *raw = strdup(b.data);           /* the parse is in place: keep a copy for the disk cache */
    sched_t *s = sched_parse(b.data);   /* takes ownership of b.data */
    if (!s) { free(raw); return; }
    have_live_schedule = 1;
    if (raw && (!last_generated[0] || strcmp(s->generated, last_generated) > 0)) write_file(SCHEDULE_CACHE, raw, strlen(raw));
    free(raw);
    if (last_generated[0] && strcmp(s->generated, last_generated) <= 0) { sched_free(s); return; }
    snprintf(last_generated, sizeof last_generated, "%s", s->generated);
    LOG("schedule: %d stations, generated %s (%lld ms)", s->n, s->generated, (long long)(now_ms() - t0));
    pthread_mutex_lock(&mu);
    if (pending) sched_free(pending);
    pending = s;
    pthread_mutex_unlock(&mu);
}

void resolver_set_presence(const char *station) {
    pthread_mutex_lock(&mu);
    snprintf(presence_want, sizeof presence_want, "%s", station ? station : "");
    pthread_mutex_unlock(&mu);
}

int resolver_network_ready(void) { return have_live_schedule; }

int resolver_count(const char *station) {
    int n = -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < ncounts; i++) if (!strcmp(counts[i].id, station)) n = counts[i].n;
    pthread_mutex_unlock(&mu);
    return n;
}

static void presence_send(net_ctx_t *net, const char *method, const char *station) {
    char url[256];
    snprintf(url, sizeof url, RTDB "/presence/%s/%s.json", station, device_id);
    int st = http_send(net, method, url, !strcmp(method, "PUT") ? "{\"ts\":{\".sv\":\"timestamp\"}}" : NULL, NULL);
    if (st != 200) LOG("presence: %s %s -> %d", method, station, st);
}

static void fetch_counts(net_ctx_t *net) {
    buf_t b = {0};
    int st = http_send(net, "GET", COUNTS_URL, NULL, &b);
    jpool *pool = NULL;
    jnode *root = st == 200 && b.data ? json_parse(b.data, &pool) : NULL;
    const jnode *c = root ? json_get(root, "counts") : NULL;
    if (c && c->t == J_OBJ) {
        pthread_mutex_lock(&mu);
        ncounts = 0;
        for (const jnode *e = c->child; e && ncounts < MAX_STATIONS; e = e->next) {
            snprintf(counts[ncounts].id, sizeof counts[ncounts].id, "%s", e->key);
            counts[ncounts].n = e->t == J_NUM ? (int)e->n : 0;
            ncounts++;
        }
        pthread_mutex_unlock(&mu);
    } else LOG("counts: fetch failed (%d)", st);
    json_free(pool);
    buf_free(&b);
}

/* The first of want[0..limit) that is missing or about to go stale. The tuned
 * station (want[0]) retries a failure after 5 s (Wi-Fi may just be coming up);
 * the rest after a minute. Caller frees. */
static char *next_want(int limit) {
    char *todo = NULL;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nwant && i < limit && !todo; i++) {
        entry_t *e = find(want[i]);
        if (!e || (!e->failed && now_ms() - e->at > FRESH_MS - 120000) ||
            (e->failed && now_ms() - e->at > (i == 0 ? 5000 : 60000)))
            todo = strdup(want[i]);
    }
    pthread_mutex_unlock(&mu);
    return todo;
}

static void resolve_one(net_ctx_t *net, char *todo) {
    pthread_mutex_lock(&mu);
    active = todo;
    pthread_mutex_unlock(&mu);
    probe_yielded = 0;
    char final[2048];
    int64_t t0 = now_ms();
    if (http_resolve(net, todo, final, sizeof final) == 0) {
        probe_t pr;
        int64_t t1 = now_ms();
        probe_any(net, todo, final, &pr);
        if (probe_yielded) LOG("resolve: gave way to the tuned station; will retry %.60s", final);
        else store(todo, final, &pr);
        static const char *fmts[] = {"unknown", "mp3", "mp4/aac", "wav"};
        LOG("resolve: %lld ms (+probe %lld ms: %s%s) -> %.60s",
            (long long)(t1 - t0), (long long)(now_ms() - t1), fmts[pr.fmt],
            pr.fmt == FMT_MP3 && pr.mp3.has_toc ? " VBR+toc" : "", final);
    } else {
        store(todo, NULL, NULL);
    }
    pthread_mutex_lock(&mu);
    active = NULL;
    pthread_mutex_unlock(&mu);
    free(todo);
}

static void *run(void *arg) {
    thread_background();
    (void)arg;
    net_ctx_t *net = net_new(app_file("cacert.pem"));
    if (!net) { LOG("resolver: no TLS context"); return NULL; }
    /* session device id, like the phone's: 'c' + random base36 (stable while running) */
    unsigned char rnd[8] = {0};
    FILE *ur = fopen("/dev/urandom", "r");
    if (ur) { if (fread(rnd, 1, sizeof rnd, ur) != sizeof rnd) rnd[0] ^= (unsigned char)now_ms(); fclose(ur); }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | rnd[i];
    device_id[0] = 'c';
    for (int i = 1; i < 14; i++) { device_id[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]; v /= 36; }
    device_id[14] = 0;
    int64_t next_schedule = 0, next_beat = 0, next_counts = 0;
    load_cached_schedule();
    /* Priorities (each handshake is 0.1-3 s on this CPU, so order IS latency):
     *  1. the tuned station's episode: audio needs it, and at boot the cached
     *     schedule already names it
     *  2. the live schedule (audio also waits for it)
     *  3. art for the dial
     *  4. presence + listener counts
     *  5. everything else wanted: the next episode, the neighbors */
    for (;;) {
        char *todo = next_want(1);
        if (todo) { resolve_one(net, todo); continue; }
        if (now_ms() >= next_schedule) {
            fetch_schedule(net);
            next_schedule = now_ms() + (have_live_schedule ? SCHEDULE_EVERY_MS : 3000);   /* Wi-Fi may not be up yet */
            continue;
        }
        pthread_mutex_lock(&mu);
        int slots_due = have_live_schedule && now_ms() >= slots_at;
        pthread_mutex_unlock(&mu);
        if (slots_due) {
            int ok = fetch_slots(net);
            pthread_mutex_lock(&mu);
            slots_at = now_ms() + (ok ? SLOTS_EVERY_MS : 60000);
            pthread_mutex_unlock(&mu);
            continue;
        }
        if (fetch_one_art(net)) continue;
        /* presence: leave the old station, join the new one, heartbeat */
        char pw[24];
        pthread_mutex_lock(&mu);
        snprintf(pw, sizeof pw, "%s", presence_want);
        pthread_mutex_unlock(&mu);
        if (strcmp(pw, presence_on)) {
            if (presence_on[0]) presence_send(net, "DELETE", presence_on);
            snprintf(presence_on, sizeof presence_on, "%s", pw);
            next_beat = 0;
            next_counts = 0;   /* show the new station's count promptly */
        }
        if (presence_on[0] && now_ms() >= next_beat) {
            presence_send(net, "PUT", presence_on);
            next_beat = now_ms() + HEARTBEAT_MS;
        }
        if (now_ms() >= next_counts) {
            fetch_counts(net);
            next_counts = now_ms() + HEARTBEAT_MS;
        }
        todo = next_want(WANT_N);
        if (!todo) { sleep(1); continue; }
        resolve_one(net, todo);
    }
    return NULL;
}

int resolver_start(void) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);   /* TLS handshakes are stack-hungry */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    return pthread_create(&th, &a, run, NULL);
}
