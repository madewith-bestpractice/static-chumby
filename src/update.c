/* Self-updater for installed copies (never for a copy running from a USB stick).
 *
 * Every 12 h (first check ~2 min after start) it reads the latest release's
 * manifest, UPDATE_MANIFEST_URL = {"version","url","sha256","size"}. If that
 * version is newer, it downloads the release zip into STATIC_DIR/update/,
 * checks its size and SHA-256, unzips it to update/stage/, and makes sure the
 * staged binary runs and reports that version. Then it marks it ready.
 *
 * The client applies it later, quietly, around 4 a.m. or when tapped in the
 * system menu: it exits with the "update" request, and boot.sh swaps the files
 * in (keeping the old binary as static-radio.prev) and restarts. If the new
 * build keeps crashing, boot.sh puts the old one back and records the version
 * in update/skip so it isn't fetched again. */
#include "common.h"
#include "json.h"
#include "net.h"
#include "update.h"
#include <errno.h>
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define UPD_DIR STATIC_DIR "/update"
#define CHECK_EVERY_S (12 * 3600)
#define FIRST_CHECK_S 120

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int state = UPD_IDLE, check_requested;
static char ready_ver[32];

int update_state(char *ver, size_t cap) {
    pthread_mutex_lock(&mu);
    int s = state;
    if (ver && cap) snprintf(ver, cap, "%s", ready_ver);
    pthread_mutex_unlock(&mu);
    return s;
}

static void set_state(int s, const char *ver) {
    pthread_mutex_lock(&mu);
    state = s;
    if (ver) snprintf(ready_ver, sizeof ready_ver, "%s", ver);
    pthread_mutex_unlock(&mu);
}

void update_check_now(void) {
    pthread_mutex_lock(&mu);
    check_requested = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

/* "1.2.10" > "1.2.9"; missing parts count as 0; anything after '-' is ignored. */
int update_version_newer(const char *a, const char *b) {
    for (int i = 0; i < 3; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x > y;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static int read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

static int write_line(const char *path, const char *s) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", s);
    if (fclose(f)) return -1;
    return rename(tmp, path);
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* Download url to path, hashing as it goes. 0 if size and hash match. */
static int download(net_ctx_t *net, const char *url, const char *path, int64_t want_size, const char *want_sha) {
    char final[2048];
    if (http_resolve(net, url, final, sizeof final)) { LOG("update: can't resolve %s", url); return -1; }
    int status = 0;
    int64_t total = -1;
    net_stream_t *s = http_stream_open(net, final, 0, -1, &status, &total);
    if (!s) { LOG("update: download failed to open (%d)", status); return -1; }
    FILE *f = fopen(path, "w");
    if (!f) { http_stream_close(s); return -1; }
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    static char buf[16384];
    int64_t got = 0;
    int n, err = 0;
    while ((n = http_stream_read(s, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { err = 1; break; }
        mbedtls_sha256_update(&h, (const unsigned char *)buf, (size_t)n);
        got += n;
        if (want_size > 0 && got > want_size) { err = 1; break; }
        usleep(2000);   /* stay polite: audio shares this CPU and link */
    }
    if (n < 0) err = 1;
    http_stream_close(s);
    if (fclose(f)) err = 1;
    unsigned char d[32];
    char dh[65];
    mbedtls_sha256_finish(&h, d);
    mbedtls_sha256_free(&h);
    hex(d, 32, dh);
    if (err || (want_size > 0 && got != want_size) || strcasecmp(dh, want_sha)) {
        LOG("update: download mismatch (%lld bytes, sha %s, expected %lld / %s)",
            (long long)got, dh, (long long)want_size, want_sha);
        return -1;
    }
    return 0;
}

static void check_once(void) {
    char skip[32] = "", ready[32] = "";
    mkdir(UPD_DIR, 0755);
    read_line(UPD_DIR "/skip", skip, sizeof skip);
    int have_ready = read_line(UPD_DIR "/ready", ready, sizeof ready) == 0 && update_version_newer(ready, STATIC_VERSION);
    set_state(have_ready ? UPD_READY : UPD_CHECKING, have_ready ? ready : NULL);
    net_ctx_t *net = net_new(app_file("cacert.pem"));
    if (!net) { set_state(UPD_ERROR, NULL); return; }
    buf_t b = {0};
    int st = http_get(net, UPDATE_MANIFEST_URL, 8192, &b);
    jpool *pool = NULL;
    const jnode *j = st == 200 && b.data ? json_parse(b.data, &pool) : NULL;
    const char *ver = j ? json_str(j, "version", NULL) : NULL;
    const char *url = j ? json_str(j, "url", NULL) : NULL;
    const char *sha = j ? json_str(j, "sha256", NULL) : NULL;
    int64_t size = j ? (int64_t)json_num(j, "size", 0) : 0;
    if (!ver || !url || !sha || strlen(sha) != 64) {
        LOG("update: no usable manifest (HTTP %d)", st);
        if (!have_ready) set_state(UPD_ERROR, NULL);   /* else keep what's staged */
    } else if (!update_version_newer(ver, STATIC_VERSION) || !strcmp(ver, skip)) {
        set_state(UPD_CURRENT, NULL);
    } else if (have_ready && !strcmp(ver, ready)) {
        /* already staged */
    } else {
        LOG("update: %s available (running %s); downloading", ver, STATIC_VERSION);
        set_state(UPD_DOWNLOADING, ver);
        int ok = download(net, url, UPD_DIR "/pkg.zip", size, sha) == 0;
        if (ok) {
            /* unpack, then make sure the staged binary runs and is what it claims */
            ok = system("rm -rf " UPD_DIR "/stage && mkdir -p " UPD_DIR "/stage && "
                        "unzip -q -o " UPD_DIR "/pkg.zip -d " UPD_DIR "/stage && "
                        "chmod 755 " UPD_DIR "/stage/static/static-radio " UPD_DIR "/stage/static/ffmpeg") == 0;
            char got[64] = "";
            FILE *p = ok ? popen(UPD_DIR "/stage/static/static-radio --version", "r") : NULL;
            if (p) { if (!fgets(got, sizeof got, p)) got[0] = 0; pclose(p); got[strcspn(got, "\r\n")] = 0; }
            ok = ok && !strcmp(got, ver);
            if (!ok) LOG("update: staged build failed its check (reports '%s')", got);
        }
        unlink(UPD_DIR "/pkg.zip");
        if (ok && write_line(UPD_DIR "/ready", ver) == 0) {
            LOG("update: %s staged; installs at the next quiet moment", ver);
            set_state(UPD_READY, ver);
        } else {
            if (system("rm -rf " UPD_DIR "/stage")) {}
            set_state(UPD_ERROR, NULL);
        }
    }
    json_free(pool);
    buf_free(&b);
    net_free(net);
}

static void *run(void *arg) {
    (void)arg;
    int64_t next = now_ms() + FIRST_CHECK_S * 1000LL;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!check_requested && now_ms() < next) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 30;
            pthread_cond_timedwait(&cv, &mu, &ts);
        }
        check_requested = 0;
        pthread_mutex_unlock(&mu);
        check_once();   /* also replaces a staged build when a newer one is out */
        next = now_ms() + CHECK_EVERY_S * 1000LL;
    }
    return NULL;
}

void update_start(void) {
    if (!app_installed()) { set_state(UPD_USB, NULL); return; }   /* a stick updates by replacing it */
    if (access(STATIC_DIR "/noupdate", F_OK) == 0) { set_state(UPD_OFF, NULL); return; }
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &a, run, NULL);
}
