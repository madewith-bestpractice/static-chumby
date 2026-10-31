#include "common.h"
#include "fetch.h"
#include "net.h"
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

typedef struct { char url[2048]; int64_t from; int fd; } job_t;

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;   /* EPIPE: the decoder is gone */
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void *run(void *arg) {
    job_t *j = arg;
    net_ctx_t *net = net_new(app_file("cacert.pem"));
    int64_t pos = j->from, t0 = now_ms();
    int attempts = 0, first = 1;
    char buf[16384];
    while (net && attempts < 4) {
        int st; int64_t total;
        net_stream_t *s = http_stream_open(net, j->url, pos, -1, &st, &total);
        if (!s || (st != 206 && !(st == 200 && pos == 0))) {
            /* (a 200 to a ranged request would restart the file: not usable) */
            LOG("fetch: open failed (%d) at %lld for %.60s", st, (long long)pos, j->url);
            http_stream_close(s);
            if (st >= 400 && st < 500) break;   /* expired/forbidden: re-resolve, don't retry */
            attempts++;
            sleep(1);
            continue;
        }
        if (first) { LOG("fetch: first byte after %lld ms (status %d)", (long long)(now_ms() - t0), st); first = 0; }
        int r;
        while ((r = http_stream_read(s, buf, sizeof buf)) > 0) {
            if (write_all(j->fd, buf, (size_t)r)) { http_stream_close(s); goto done; }
            pos += r;
            attempts = 0;
        }
        http_stream_close(s);
        if (r == 0) break;   /* end of file */
        attempts++;          /* dropped: reconnect from pos */
        LOG("fetch: connection dropped at %lld, reconnecting", (long long)pos);
    }
done:
    close(j->fd);
    net_free(net);
    free(j);
    return NULL;
}

int fetch_start(const char *url, int64_t from, int fd) {
    job_t *j = calloc(1, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", url);
    j->from = from;
    j->fd = fd;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, run, j)) { close(fd); free(j); return -1; }
    return 0;
}

/* ---------- neighbor pre-buffer ---------- */

struct prebuf {
    pthread_mutex_t mu;
    char url[2048];
    uint8_t *ring;
    size_t cap;
    int64_t lo, hi;          /* bytes [lo, hi) are in the ring (at index byte % cap) */
    int64_t limit;           /* don't read past this until attached */
    int out_fd;              /* >= 0 once attached */
    int64_t out_pos;
    int cancel;
    int dead;   /* the thread gave up while unattached: the owner frees on cancel */
};

static void *pb_run(void *arg) {
    prebuf_t *p = arg;
    net_ctx_t *net = net_new(app_file("cacert.pem"));
    char buf[8192];
    int attempts = 0;
    while (net && attempts < 4) {
        pthread_mutex_lock(&p->mu);
        int64_t pos = p->hi;
        int cancel = p->cancel;
        pthread_mutex_unlock(&p->mu);
        if (cancel) break;
        int st; int64_t total;
        net_stream_t *s = http_stream_open(net, p->url, pos, -1, &st, &total);
        if (!s || st != 206) { http_stream_close(s); attempts++; sleep(1); continue; }
        for (;;) {
            /* pace: until attached, stay within the ahead limit */
            pthread_mutex_lock(&p->mu);
            while (p->out_fd < 0 && !p->cancel && p->hi >= p->limit) {
                pthread_mutex_unlock(&p->mu);
                usleep(200000);
                pthread_mutex_lock(&p->mu);
            }
            cancel = p->cancel && p->out_fd < 0;
            pthread_mutex_unlock(&p->mu);
            if (cancel) { http_stream_close(s); goto done; }
            int r = http_stream_read(s, buf, sizeof buf);
            if (r <= 0) { http_stream_close(s); if (r == 0) goto drain; attempts++; break; }
            attempts = 0;
            pthread_mutex_lock(&p->mu);
            for (int i = 0; i < r; i++) p->ring[(p->hi + i) % p->cap] = (uint8_t)buf[i];
            p->hi += r;
            if (p->hi - p->lo > (int64_t)p->cap) p->lo = p->hi - (int64_t)p->cap;
            int fd = p->out_fd;
            pthread_mutex_unlock(&p->mu);
            if (fd >= 0) {   /* attached: flush everything from out_pos to hi */
                for (;;) {
                    pthread_mutex_lock(&p->mu);
                    int64_t from = p->out_pos, to = p->hi;
                    size_t n = (size_t)(to - from);
                    if (n > sizeof buf) n = sizeof buf;
                    char out[8192];
                    for (size_t i = 0; i < n; i++) out[i] = (char)p->ring[(from + (int64_t)i) % p->cap];
                    pthread_mutex_unlock(&p->mu);
                    if (!n) break;
                    if (write_all(fd, out, n)) { http_stream_close(s); goto done; }
                    pthread_mutex_lock(&p->mu);
                    p->out_pos += (int64_t)n;
                    pthread_mutex_unlock(&p->mu);
                }
            }
        }
    }
drain:;
    /* EOF: give an attached player whatever is left */
    pthread_mutex_lock(&p->mu);
    int fd = p->out_fd;
    pthread_mutex_unlock(&p->mu);
    if (fd >= 0) {
        for (;;) {
            char out[8192];
            pthread_mutex_lock(&p->mu);
            size_t n = (size_t)(p->hi - p->out_pos);
            if (n > sizeof out) n = sizeof out;
            for (size_t i = 0; i < n; i++) out[i] = (char)p->ring[(p->out_pos + (int64_t)i) % p->cap];
            p->out_pos += (int64_t)n;
            pthread_mutex_unlock(&p->mu);
            if (!n || write_all(fd, out, n)) break;
        }
    }
done:
    net_free(net);
    pthread_mutex_lock(&p->mu);
    fd = p->out_fd;
    if (fd < 0 && !p->cancel) {
        /* Gave up (e.g. an expired link) while the audio engine still holds p:
         * freeing here was a use-after-free that hung the main thread on a
         * mutex in freed memory. Stay allocated; prebuf_cancel() frees. */
        p->dead = 1;
        pthread_mutex_unlock(&p->mu);
        return NULL;
    }
    pthread_mutex_unlock(&p->mu);
    if (fd >= 0) close(fd);
    free(p->ring);
    pthread_mutex_destroy(&p->mu);
    free(p);
    return NULL;
}

prebuf_t *prebuf_start(const char *url, int64_t start, int64_t ahead_limit, size_t cap) {
    prebuf_t *p = calloc(1, sizeof *p);
    p->ring = malloc(cap);
    if (!p->ring) { free(p); return NULL; }
    pthread_mutex_init(&p->mu, NULL);
    snprintf(p->url, sizeof p->url, "%s", url);
    p->cap = cap;
    p->lo = p->hi = start;
    p->limit = ahead_limit;
    p->out_fd = -1;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, pb_run, p)) { free(p->ring); free(p); return NULL; }
    return p;
}

void prebuf_set_limit(prebuf_t *p, int64_t limit) {
    pthread_mutex_lock(&p->mu);
    p->limit = limit;
    pthread_mutex_unlock(&p->mu);
}

void prebuf_window(prebuf_t *p, int64_t *lo, int64_t *hi) {
    pthread_mutex_lock(&p->mu);
    *lo = p->lo; *hi = p->hi;
    pthread_mutex_unlock(&p->mu);
}

int prebuf_attach(prebuf_t *p, int64_t from, int fd) {
    pthread_mutex_lock(&p->mu);
    int ok = !p->cancel && !p->dead && p->out_fd < 0 && from >= p->lo && from <= p->hi;
    if (ok) { p->out_fd = fd; p->out_pos = from; }
    pthread_mutex_unlock(&p->mu);
    return ok ? 0 : -1;
}

void prebuf_cancel(prebuf_t *p) {
    pthread_mutex_lock(&p->mu);
    if (p->dead) {   /* its thread already exited: we own the memory now */
        pthread_mutex_unlock(&p->mu);
        free(p->ring);
        pthread_mutex_destroy(&p->mu);
        free(p);
        return;
    }
    p->cancel = 1;
    pthread_mutex_unlock(&p->mu);
}
