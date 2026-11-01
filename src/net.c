/* Minimal HTTP/1.1 client over mbedTLS (the chumby's own OpenSSL is from 2004).
 * Connection: close, so bodies are read to EOF; chunked bodies are decoded. */
#include "common.h"
#include "net.h"
#include <ctype.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <pthread.h>

struct net_ctx {
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;     /* TLS 1.2 only: resumption costs no public-key math */
    mbedtls_ssl_config conf13;   /* default (1.3 preferred), for hosts that refuse 1.2 */
};

/* Hosts that failed a TLS 1.2 handshake: use conf13 for them from then on. */
#define TLS13_N 16
static char tls13_host[TLS13_N][128];
static int tls13_next;
static pthread_mutex_t tls13_mu = PTHREAD_MUTEX_INITIALIZER;
static int host_needs13(const char *h) {
    int r = 0;
    pthread_mutex_lock(&tls13_mu);
    for (int i = 0; i < TLS13_N && !r; i++) r = !strcmp(tls13_host[i], h);
    pthread_mutex_unlock(&tls13_mu);
    return r;
}
static void host_mark13(const char *h) {
    pthread_mutex_lock(&tls13_mu);
    snprintf(tls13_host[tls13_next], sizeof tls13_host[0], "%s", h);
    tls13_next = (tls13_next + 1) % TLS13_N;
    pthread_mutex_unlock(&tls13_mu);
}

/* P-256 first: mbedTLS's NIST fast reduction + fixed-point comb make it the
 * cheapest key exchange here (no FPU, 454 MHz). */
static const uint16_t tls_groups[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1, MBEDTLS_SSL_IANA_TLS_GROUP_NONE };

/* The CA bundle is parsed ONCE (333 KB of PEM is seconds of CPU here) and
 * shared read-only by every thread's context. */
static mbedtls_x509_crt shared_ca;
static int shared_ca_state;   /* 0 unparsed, 1 ok, -1 failed */
static pthread_mutex_t ca_mu = PTHREAD_MUTEX_INITIALIZER;

static mbedtls_x509_crt *ca_chain(const char *ca_file) {
    pthread_mutex_lock(&ca_mu);
    if (!shared_ca_state) {
        mbedtls_x509_crt_init(&shared_ca);
        int bad = mbedtls_x509_crt_parse_file(&shared_ca, ca_file);
        shared_ca_state = bad < 0 ? -1 : 1;
        if (bad < 0) LOG("net: CA parse failed (%d)", bad);
    }
    pthread_mutex_unlock(&ca_mu);
    return shared_ca_state > 0 ? &shared_ca : NULL;
}

net_ctx_t *net_new(const char *ca_file) {
    mbedtls_x509_crt *ca = ca_chain(ca_file);
    if (!ca) return NULL;
    net_ctx_t *c = calloc(1, sizeof *c);
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    const char *pers = "static-chumby";
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        LOG("net: drbg seed failed");
        return NULL;
    }
    mbedtls_ssl_config *confs[2] = { &c->conf, &c->conf13 };
    for (int i = 0; i < 2; i++) {
        mbedtls_ssl_config *cf = confs[i];
        mbedtls_ssl_config_init(cf);
        mbedtls_ssl_config_defaults(cf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode(cf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(cf, ca, NULL);
        mbedtls_ssl_conf_rng(cf, mbedtls_ctr_drbg_random, &c->drbg);
        mbedtls_ssl_conf_read_timeout(cf, 20000);
        mbedtls_ssl_conf_groups(cf, tls_groups);
    }
    /* TLS 1.3 resumption still does a full ECDHE (servers only offer psk_dhe_ke):
     * ~0.5 s here, every 30 s for presence. A 1.2 resumption is ~0.1 s. */
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    return c;
}

void net_free(net_ctx_t *c) {
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ssl_config_free(&c->conf13);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->ent);
    free(c);
}

void buf_free(buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int buf_put(buf_t *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->len + n + 1) nc *= 2;
        char *d = realloc(b->data, nc);
        if (!d) return -1;
        b->data = d; b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

typedef struct { int https; char host[256]; char port[8]; char path[2048]; } url_t;

static int parse_url(const char *u, url_t *o) {
    memset(o, 0, sizeof *o);
    const char *p;
    if (!strncmp(u, "https://", 8)) { o->https = 1; p = u + 8; strcpy(o->port, "443"); }
    else if (!strncmp(u, "http://", 7)) { p = u + 7; strcpy(o->port, "80"); }
    else return -1;
    size_t hl = strcspn(p, "/:?#");
    if (hl == 0 || hl >= sizeof o->host) return -1;
    memcpy(o->host, p, hl);
    p += hl;
    if (*p == ':') {
        size_t pl = strcspn(p + 1, "/?#");
        if (pl == 0 || pl >= sizeof o->port) return -1;
        memcpy(o->port, p + 1, pl); o->port[pl] = 0;
        p += 1 + pl;
    }
    if (*p != '/') { o->path[0] = '/'; strncpy(o->path + 1, p, sizeof o->path - 2); }
    else strncpy(o->path, p, sizeof o->path - 1);
    char *hash = strchr(o->path, '#');
    if (hash) *hash = 0;
    return 0;
}

/* Resolve a Location header against the current URL. */
static void join_location(const url_t *cur, const char *loc, char *out, size_t cap) {
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) snprintf(out, cap, "%s", loc);
    else if (!strncmp(loc, "//", 2)) snprintf(out, cap, "%s:%s", cur->https ? "https" : "http", loc);
    else if (loc[0] == '/') snprintf(out, cap, "%s://%s%s%s%s", cur->https ? "https" : "http", cur->host,
                                     strcmp(cur->port, cur->https ? "443" : "80") ? ":" : "",
                                     strcmp(cur->port, cur->https ? "443" : "80") ? cur->port : "", loc);
    else {
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", cur->path);
        char *q = strchr(dir, '?'); if (q) *q = 0;
        char *s = strrchr(dir, '/'); if (s) s[1] = 0;
        snprintf(out, cap, "%s://%s%s%s", cur->https ? "https" : "http", cur->host, dir, loc);
    }
}

typedef struct {
    mbedtls_net_context fd;
    mbedtls_ssl_context ssl;
    int https, shook;
    char host[256];
} conn_t;

/* TLS session resumption: a full handshake is ~1-2 s of CPU here (certificate
 * chain + key exchange); a resumed one skips the certificate work (TLS 1.3
 * PSK) or nearly everything (TLS 1.2 tickets). One cached session per host,
 * shared by all network threads. */
#define SESS_N 24
static struct { char host[256]; mbedtls_ssl_session s; int valid; } sess[SESS_N];
static int sess_next;
static pthread_mutex_t sess_mu = PTHREAD_MUTEX_INITIALIZER;

static void sess_offer(conn_t *k) {
    pthread_mutex_lock(&sess_mu);
    for (int i = 0; i < SESS_N; i++)
        if (sess[i].valid && !strcmp(sess[i].host, k->host)) { mbedtls_ssl_set_session(&k->ssl, &sess[i].s); break; }
    pthread_mutex_unlock(&sess_mu);
}

static void sess_save(conn_t *k) {
    mbedtls_ssl_session t;
    mbedtls_ssl_session_init(&t);
    if (mbedtls_ssl_get_session(&k->ssl, &t) != 0) { mbedtls_ssl_session_free(&t); return; }
    pthread_mutex_lock(&sess_mu);
    int slot = -1;
    for (int i = 0; i < SESS_N && slot < 0; i++) if (sess[i].valid && !strcmp(sess[i].host, k->host)) slot = i;
    if (slot < 0) { slot = sess_next; sess_next = (sess_next + 1) % SESS_N; }
    if (sess[slot].valid) mbedtls_ssl_session_free(&sess[slot].s);
    snprintf(sess[slot].host, sizeof sess[slot].host, "%s", k->host);
    sess[slot].s = t;   /* the struct now owns t's buffers */
    sess[slot].valid = 1;
    pthread_mutex_unlock(&sess_mu);
}

/* Persist the session cache so a restart (update, respawn) or a quick power
 * cycle resumes instead of doing full handshakes. Saved rarely (every 30 min
 * and at exit) to spare the flash. File: [u16 host len][host][u32 len][session]... */
void net_sessions_save(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    static unsigned char b[4096];
    int n = 0;
    pthread_mutex_lock(&sess_mu);
    for (int i = 0; i < SESS_N; i++) {
        size_t len = 0;
        if (!sess[i].valid || mbedtls_ssl_session_save(&sess[i].s, b, sizeof b, &len) != 0) continue;
        uint16_t hl = (uint16_t)strlen(sess[i].host);
        uint32_t sl = (uint32_t)len;
        fwrite(&hl, 2, 1, f); fwrite(sess[i].host, 1, hl, f);
        fwrite(&sl, 4, 1, f); fwrite(b, 1, len, f);
        n++;
    }
    pthread_mutex_unlock(&sess_mu);
    if (fclose(f) == 0) rename(tmp, path);
    LOG("net: saved %d TLS sessions", n);
}

void net_sessions_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    static unsigned char b[4096];
    int n = 0;
    pthread_mutex_lock(&sess_mu);
    for (;;) {
        uint16_t hl; uint32_t sl;
        char host[256];
        if (fread(&hl, 2, 1, f) != 1 || hl >= sizeof host || fread(host, 1, hl, f) != hl) break;
        host[hl] = 0;
        if (fread(&sl, 4, 1, f) != 1 || sl > sizeof b || fread(b, 1, sl, f) != sl) break;
        if (sess_next >= SESS_N) break;
        int slot = sess_next;
        mbedtls_ssl_session_init(&sess[slot].s);
        if (mbedtls_ssl_session_load(&sess[slot].s, b, sl) != 0) { mbedtls_ssl_session_free(&sess[slot].s); continue; }
        snprintf(sess[slot].host, sizeof sess[slot].host, "%s", host);
        sess[slot].valid = 1;
        sess_next++;
        n++;
    }
    sess_next %= SESS_N;
    pthread_mutex_unlock(&sess_mu);
    fclose(f);
    LOG("net: loaded %d TLS sessions", n);
}

static int conn_open(net_ctx_t *c, conn_t *k, const url_t *u) {
    mbedtls_net_init(&k->fd);
    mbedtls_ssl_init(&k->ssl);
    k->https = u->https;
    k->shook = 0;
    snprintf(k->host, sizeof k->host, "%s", u->host);
    int r = mbedtls_net_connect(&k->fd, u->host, u->port, MBEDTLS_NET_PROTO_TCP);
    if (r) { LOG("net: connect %s:%s failed (%d)", u->host, u->port, r); return -1; }
    if (!u->https) return 0;
    int use13 = host_needs13(u->host);
    if ((r = mbedtls_ssl_setup(&k->ssl, use13 ? &c->conf13 : &c->conf)) ||
        (r = mbedtls_ssl_set_hostname(&k->ssl, u->host))) return -1;
    mbedtls_ssl_set_bio(&k->ssl, &k->fd, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
    sess_offer(k);
    int64_t t0 = now_ms();
    while ((r = mbedtls_ssl_handshake(&k->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char e[120]; mbedtls_strerror(r, e, sizeof e);
            LOG("net: TLS handshake with %s failed: %s", u->host, e);
            if (!use13 && (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE || r == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION ||
                           r == MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE)) {   /* a 1.3-only host: retry once with 1.3 */
                host_mark13(u->host);
                mbedtls_ssl_free(&k->ssl);
                mbedtls_net_free(&k->fd);
                return conn_open(c, k, u);
            }
            return -1;
        }
    }
    k->shook = 1;
    int64_t took = now_ms() - t0;
    LOG("net: TLS %s %s in %lld ms", u->host, mbedtls_ssl_get_version(&k->ssl), (long long)took);
    return 0;
}

static int conn_write(conn_t *k, const char *p, size_t n) {
    while (n) {
        int r = k->https ? mbedtls_ssl_write(&k->ssl, (const unsigned char *)p, n)
                         : mbedtls_net_send(&k->fd, (const unsigned char *)p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* >0 bytes, 0 EOF, <0 error */
static int conn_read(conn_t *k, char *p, size_t n) {
    for (;;) {
        int r = k->https ? mbedtls_ssl_read(&k->ssl, (unsigned char *)p, n)
                         : mbedtls_net_recv_timeout(&k->fd, (unsigned char *)p, n, 20000);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
}

static void conn_close(conn_t *k) {
    if (k->https && k->shook) sess_save(k);   /* the newest ticket for next time */
    if (k->https) mbedtls_ssl_close_notify(&k->ssl);
    mbedtls_ssl_free(&k->ssl);
    mbedtls_net_free(&k->fd);
}

/* Case-insensitive header lookup in a NUL-terminated header block. */
static int header(const char *hdrs, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *line = hdrs; line && *line; ) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - line) > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(eol - v);
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return 1;
        }
        line = eol + 2;
    }
    return 0;
}

static int dechunk(buf_t *b) {
    buf_t o = {0};
    size_t i = 0;
    while (i < b->len) {
        char *end;
        unsigned long sz = strtoul(b->data + i, &end, 16);
        char *crlf = strstr(end, "\r\n");
        if (!crlf) break;
        i = (size_t)(crlf - b->data) + 2;
        if (sz == 0) break;
        if (i + sz > b->len) sz = b->len - i;
        buf_put(&o, b->data + i, sz);
        i += sz + 2;
    }
    buf_free(b);
    *b = o;
    return 0;
}

/* One request. headers_only: stop after the header block (for redirect probing).
 * Returns status; fills location (if 3xx) and body. */
static int request(net_ctx_t *c, const char *method, const char *url, const char *send_body,
                   int headers_only, size_t max_body, buf_t *body, char *location, size_t loc_cap) {
    url_t u;
    if (parse_url(url, &u)) { LOG("net: bad url %.80s", url); return -1; }
    conn_t k;
    if (conn_open(c, &k, &u)) { conn_close(&k); return -1; }
    char req[3072], extra[96] = "";
    size_t blen = send_body ? strlen(send_body) : 0;
    if (send_body) snprintf(extra, sizeof extra, "Content-Type: application/json\r\nContent-Length: %zu\r\n", blen);
    int n = snprintf(req, sizeof req,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n%s%s\r\n",
        method, u.path, u.host, headers_only ? "Range: bytes=0-1\r\n" : "", extra);
    if (n >= (int)sizeof req || conn_write(&k, req, (size_t)n) ||
        (blen && conn_write(&k, send_body, blen))) { conn_close(&k); return -1; }

    buf_t raw = {0};
    char tmp[8192];
    size_t body_at = 0;   /* offset (not a pointer: buf_put reallocs) */
    for (;;) {
        int r = conn_read(&k, tmp, sizeof tmp);
        if (r <= 0) break;
        buf_put(&raw, tmp, (size_t)r);
        if (!body_at) {
            char *h = strstr(raw.data, "\r\n\r\n");
            if (h) body_at = (size_t)(h - raw.data) + 4;
        }
        if (body_at && headers_only) break;
        if (body_at && raw.len - body_at > max_body) { LOG("net: body too large"); break; }
    }
    conn_close(&k);
    char *hend;
    if (!raw.data || !(hend = strstr(raw.data, "\r\n\r\n"))) { buf_free(&raw); return -1; }
    hend[2] = 0;   /* terminate the header block after its last CRLF */
    int status = 0;
    sscanf(raw.data, "HTTP/%*s %d", &status);
    if (location) {
        location[0] = 0;
        char loc[2048];
        if (status >= 300 && status < 400 && header(raw.data, "Location", loc, sizeof loc))
            join_location(&u, loc, location, loc_cap);
    }
    if (body && !headers_only) {
        char te[64] = "";
        header(raw.data, "Transfer-Encoding", te, sizeof te);
        buf_put(body, hend + 4, raw.len - (size_t)(hend + 4 - raw.data));
        if (strcasestr(te, "chunked")) dechunk(body);
    }
    buf_free(&raw);
    return status;
}

int http_get(net_ctx_t *c, const char *url, size_t max_body, buf_t *out) {
    char cur[2048], next[2048];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 10; hop++) {
        buf_free(out);
        int st = request(c, "GET", cur, NULL, 0, max_body, out, next, sizeof next);
        if (st >= 300 && st < 400 && next[0]) { snprintf(cur, sizeof cur, "%s", next); continue; }
        return st;
    }
    return -1;
}

int http_resolve(net_ctx_t *c, const char *url, char *final, size_t cap) {
    char cur[2048], next[2048];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 10; hop++) {
        int st = request(c, "GET", cur, NULL, 1, 0, NULL, next, sizeof next);
        if (st >= 300 && st < 400 && next[0]) { snprintf(cur, sizeof cur, "%s", next); continue; }
        if (st >= 200 && st < 300) { snprintf(final, cap, "%s", cur); return 0; }
        LOG("net: resolve ended with status %d at %.80s", st, cur);
        return -1;
    }
    return -1;
}

int http_send(net_ctx_t *c, const char *method, const char *url, const char *json_body, buf_t *out) {
    buf_t tmp = {0};
    int st = request(c, method, url, json_body, 0, 1 << 20, out ? out : &tmp, NULL, 0);
    buf_free(&tmp);
    return st;
}

/* ---------- streaming ranged GET ---------- */

struct net_stream {
    conn_t k;
    char eff[2048];      /* the URL that answered (after any redirects) */
    char pre[16384];     /* body bytes that arrived with the headers */
    size_t pre_len, pre_off;
};

/* One request. A 3xx fills `loc` (absolute) and returns NULL. */
static net_stream_t *stream_open1(net_ctx_t *c, const char *url, int64_t from, int64_t to,
                                  int *status, int64_t *total, char *loc, size_t loc_cap) {
    url_t u;
    *status = -1; *total = 0;
    loc[0] = 0;
    if (parse_url(url, &u)) return NULL;
    net_stream_t *s = calloc(1, sizeof *s);
    if (conn_open(c, &s->k, &u)) { conn_close(&s->k); free(s); return NULL; }
    char range[64] = "";
    /* Always ask for a range, even from 0: some hosts answer a range request
     * themselves (206) but redirect a plain GET (transistor.fm), and the
     * resolver walked the chain WITH a range. */
    {
        if (to >= 0) snprintf(range, sizeof range, "Range: bytes=%lld-%lld\r\n", (long long)from, (long long)to);
        else snprintf(range, sizeof range, "Range: bytes=%lld-\r\n", (long long)from);
    }
    char req[3072];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n%s\r\n",
        u.path, u.host, range);
    if (n >= (int)sizeof req || conn_write(&s->k, req, (size_t)n)) { http_stream_close(s); return NULL; }
    /* read the header block */
    char hdr[8192];
    size_t hl = 0;
    char *end = NULL;
    while (!end && hl < sizeof hdr - 1) {
        int r = conn_read(&s->k, hdr + hl, sizeof hdr - 1 - hl);
        if (r <= 0) { http_stream_close(s); return NULL; }
        hl += (size_t)r;
        hdr[hl] = 0;
        end = strstr(hdr, "\r\n\r\n");
    }
    if (!end) { http_stream_close(s); return NULL; }
    size_t body_at = (size_t)(end - hdr) + 4;
    end[2] = 0;
    sscanf(hdr, "HTTP/%*s %d", status);
    if (*status >= 300 && *status < 400) {
        char l[2048];
        if (header(hdr, "Location", l, sizeof l)) join_location(&u, l, loc, loc_cap);
        http_stream_close(s);
        return NULL;
    }
    char v[128];
    if (header(hdr, "Content-Range", v, sizeof v)) {           /* bytes a-b/total */
        char *slash = strchr(v, '/');
        if (slash && slash[1] != '*') *total = atoll(slash + 1);
    } else if (header(hdr, "Content-Length", v, sizeof v) && *status == 200) *total = atoll(v);
    if (header(hdr, "Transfer-Encoding", v, sizeof v) && strcasestr(v, "chunked")) {
        LOG("net: chunked stream not supported: %.60s", url);
        http_stream_close(s); return NULL;
    }
    s->pre_len = hl - body_at;
    memcpy(s->pre, hdr + body_at, s->pre_len);
    snprintf(s->eff, sizeof s->eff, "%s", url);
    return s;
}

/* Follows redirects: a resolved URL can still bounce. Some hosts answer a tiny
 * range themselves (the resolver's 2-byte probe) but redirect any real read to
 * the file's actual home (transistor.fm). */
net_stream_t *http_stream_open(net_ctx_t *c, const char *url, int64_t from, int64_t to,
                               int *status, int64_t *total) {
    char cur[2048], loc[2048];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 5; hop++) {
        net_stream_t *s = stream_open1(c, cur, from, to, status, total, loc, sizeof loc);
        if (s || !loc[0]) return s;
        snprintf(cur, sizeof cur, "%s", loc);
    }
    return NULL;
}

const char *http_stream_url(net_stream_t *s) { return s->eff; }

int http_stream_read(net_stream_t *s, char *buf, size_t n) {
    if (s->pre_off < s->pre_len) {
        size_t m = s->pre_len - s->pre_off < n ? s->pre_len - s->pre_off : n;
        memcpy(buf, s->pre + s->pre_off, m);
        s->pre_off += m;
        return (int)m;
    }
    return conn_read(&s->k, buf, n);
}

void http_stream_close(net_stream_t *s) {
    if (!s) return;
    conn_close(&s->k);
    free(s);
}
