/* Audio engine. Decoders (in-process Helix threads; ffmpeg only as a fallback for
 * unknown formats) write raw s16le 44.1 kHz stereo to a pipe; THIS process decides what reaches aplay,
 * sample by sample, so playback always sits on the true live position:
 *   - audio arriving early waits (silence), late audio is dropped to catch up;
 *   - the next episode is pre-launched before the boundary and starts at it;
 *   - the 1.5 s gap is silence, bulletins stop at 30 s, nothing ever plays ahead.
 * (Listener sync is the product: same moment as every phone.) */
#include "common.h"
#include "audio.h"
#include "resolver.h"
#include "fetch.h"
#include "mp3probe.h"
#include "decode.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define RATE 44100
#define FRAME 4                        /* s16 stereo */
#define CHUNK_FRAMES 1024              /* 4096 bytes == PIPE_BUF: atomic pipe writes */
#define RING_BYTES (RATE * FRAME * 3)  /* 3 s of decoded audio per stream */
#define OUT_LATENCY_MS 850             /* pipe (64 KB ≈ 370 ms) + aplay's buffer */
#define SYNC_SLOP_MS 120
#define PRELAUNCH_MS 8000              /* start the next episode's decoder this early */
#define FADE_FRAMES (RATE / 5)         /* 200 ms fade-in */
#define MAX_STREAMS 3                  /* tuned now + tuned next + outgoing */
#define XFADE_MS 450                   /* equal-power station crossfade (the watch's) */
#define OUTGOING_MAX_MS 15000          /* stop the old station if the new never starts */

typedef struct {
    int used;
    pid_t pid;
    int fd;
    int st, slot;
    char url[2048];          /* schedule URL (identity) */
    int64_t start_ms;        /* media position of the first decoded frame */
    int64_t frames;          /* frames consumed (played or dropped) */
    uint8_t *ring;
    int rhead, rlen;
    int eof;
    int64_t got_bytes;
    int64_t launched_ms, first_pcm_ms;
    int tune_launch;         /* launched mid-episode (seeked) */
    int direct;              /* 0 ffmpeg https, 1 direct MP3, 2 direct from a pre-buffer */
    int fade;
} stream_t;

static stream_t streams[MAX_STREAMS];
static pid_t aplay_pid = -1;
static int aplay_fd = -1;
static int tuned = -1, muted;
/* Station change: the old station keeps playing until the new one has audio,
 * then they crossfade (instead of seconds of silence while tuning). */
static int outgoing = -1;
static int64_t tuned_at;
static int64_t outgoing_since, xfade_start;
/* Expected start-up per path (adapts): [0] ffmpeg's own HTTPS (+seek, +probe),
 * [1] direct MP3 (one ranged request on our TLS) — very different populations. */
/* [0] ffmpeg fallback, [1] native MP3/WAV, [2] native from a neighbor
 * pre-buffer, [3] native MP4/AAC (loads + parses the moov first) */
static int64_t lead_ms[4] = { 12000, 1500, 600, 4000 };
static const int64_t lead_min[4] = { 4000, 500, 200, 1500 }, lead_max[4] = { 25000, 6000, 3000, 10000 },
                     lead_outlier[4] = { 40000, 10000, 6000, 20000 };

/* Neighbor pre-buffers: the stations either side of the needle keep a rolling
 * window of their live MP3 bytes, so tuning to one needs no connection at all. */
#define NB_N 2
#define NB_CAP (1024 * 1024)
static struct { prebuf_t *p; int st, slot; char url[2048]; probe_t pr; } nb[NB_N];

/* Byte for `ms` into a probed file: exact for MP3 (bitrate / Xing table), an
 * estimate for MP4 (near-constant AAC bitrate) — fine for sizing a window. */
static int64_t est_byte(const probe_t *p, int64_t ms) {
    if (p->fmt == FMT_MP3) return mp3_byte_for_ms(&p->mp3, ms);
    if (p->fmt == FMT_MP4 && p->mp4_dur_ms > 0) {   /* interpolate the 256-point seek table */
        if (ms < 0) ms = 0;
        if (ms >= p->mp4_dur_ms) return p->mp4_toc[256];
        int64_t x = ms * 256;
        int i = (int)(x / p->mp4_dur_ms);
        int64_t rem = x - (int64_t)i * p->mp4_dur_ms;
        return (int64_t)p->mp4_toc[i] + ((int64_t)p->mp4_toc[i + 1] - p->mp4_toc[i]) * rem / p->mp4_dur_ms;
    }
    return -1;
}
static int nb_want[NB_N] = { -1, -1 };

static int nb_find(int st, int slot, const char *url) {
    for (int i = 0; i < NB_N; i++)
        if (nb[i].p && nb[i].st == st && nb[i].slot == slot && !strcmp(nb[i].url, url)) return i;
    return -1;
}
static int net_ready;                  /* no decoders until the network is up (boot) */
static int64_t fail_at[MAX_STATIONS];
static int state = AUDIO_IDLE;
static int16_t pending[CHUNK_FRAMES * 2];
static int have_pending;

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);   /* decoders must not inherit aplay's stdin etc. */
}

/* In a freshly forked child: die with the client, never outlive it. */
static void child_setup(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    signal(SIGPIPE, SIG_DFL);
}

static void spawn_aplay(void) {
    int p[2];
    if (pipe(p)) return;
    pid_t pid = fork();
    if (pid == 0) {
        child_setup();
        dup2(p[0], 0);
        close(p[0]); close(p[1]);
        execl("/usr/bin/aplay", "aplay", "-q", "-t", "raw", "-f", "cd", "-", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    aplay_pid = pid;
    aplay_fd = p[1];
    set_nonblock(aplay_fd);
    have_pending = 0;
    LOG("audio: aplay pid %d", pid);
}

void audio_init(void) {
    signal(SIGPIPE, SIG_IGN);
    spawn_aplay();
}

static void stream_kill(stream_t *s) {
    if (!s->used) return;
    if (s->pid > 0) kill(s->pid, SIGKILL);
    if (s->fd >= 0) close(s->fd);
    free(s->ring);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

static stream_t *find_stream(int st, int slot, const char *url) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i].used && streams[i].st == st && streams[i].slot == slot && !strcmp(streams[i].url, url))
            return &streams[i];
    return NULL;
}

static stream_t *launch(int st, int slot, const char *url, int64_t seek_ms) {
    stream_t *s = NULL;
    for (int i = 0; i < MAX_STREAMS && !s; i++) if (!streams[i].used) s = &streams[i];
    if (!s) return NULL;
    char eff[2048];
    /* Native path: a pre-resolved, probed MP3 / MP4-AAC / WAV is decoded in
     * process (Helix, fixed point) from ONE ranged request on our own TLS, or
     * straight from a neighbor pre-buffer: no ffmpeg start-up, no redirect
     * walk, no extra handshakes; ~9% CPU for MP3, ~25% for HE-AAC. */
    probe_t pr;
    if (resolver_lookup_probe(url, eff, sizeof eff, &pr)) {
        int nbi = pr.fmt == FMT_MP3 || pr.fmt == FMT_MP4 ? nb_find(st, slot, url) : -1;
        int p[2];
        if (!pipe(p)) {
            fcntl(p[1], F_SETFD, FD_CLOEXEC);
            int used_pb = 0;
            if (decode_start(eff, &pr, seek_ms, nbi >= 0 ? nb[nbi].p : NULL, p[1], &used_pb) == 0) {
                if (used_pb) nb[nbi].p = NULL;
                memset(s, 0, sizeof *s);
                s->used = 1;
                s->pid = -1;
                s->fd = p[0];
                set_nonblock(s->fd);
                s->st = st; s->slot = slot;
                snprintf(s->url, sizeof s->url, "%s", url);
                s->start_ms = seek_ms > 500 ? seek_ms : 0;
                s->ring = malloc(RING_BYTES);
                s->launched_ms = now_ms();
                s->tune_launch = seek_ms > 500;
                s->direct = used_pb ? 2 : pr.fmt == FMT_MP4 ? 3 : 1;
                static const char *fmts[] = {"?", "mp3", "aac", "wav"};
                LOG("audio: launch st=%d slot=%d seek=%lld ms native %s%s %.60s", st, slot, (long long)seek_ms,
                    fmts[pr.fmt], used_pb ? " (pre-buffered)" : "", eff);
                return s;
            }
            close(p[0]); close(p[1]);
            LOG("audio: native start failed; falling back to ffmpeg");
        }
    }
    mp3info_t mi;
    int direct = resolver_lookup_mp3(url, eff, sizeof eff, &mi);
    if (!direct && !resolver_lookup(url, eff, sizeof eff)) snprintf(eff, sizeof eff, "%s", url);
    int64_t from = direct ? (seek_ms > 500 ? mp3_byte_for_ms(&mi, seek_ms) : 0) : 0;
    int nbi = -1;   /* (the ffmpeg pipe fallback no longer takes pre-buffers) */
    int in[2] = { -1, -1 };
    if (direct && pipe(in)) direct = 0;
    if (direct) fcntl(in[1], F_SETFD, FD_CLOEXEC);   /* only the fetch thread holds the write end */
    char ss[32];
    snprintf(ss, sizeof ss, "%.3f", seek_ms / 1000.0);
    int p[2];
    if (pipe(p)) return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        child_setup();
        dup2(p[1], 1);
        if (direct) dup2(in[0], 0);
        else { int devnull = open("/dev/null", O_RDONLY); dup2(devnull, 0); }
        int err = open("/tmp/ffmpeg.err", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (err >= 0) dup2(err, 2);
        close(p[0]); close(p[1]);
        const char *argv[40];
        int a = 0;
        argv[a++] = (char *)app_file("ffmpeg");
        argv[a++] = "-hide_banner"; argv[a++] = "-nostdin"; argv[a++] = "-loglevel"; argv[a++] = "error";
        if (direct) {   /* bytes arrive on stdin, already at the right offset */
            argv[a++] = "-f"; argv[a++] = "mp3"; argv[a++] = "-i"; argv[a++] = "pipe:0";
        } else {   /* ffmpeg's own HTTPS (these are protocol options: a pipe input rejects them) */
            argv[a++] = "-ca_file"; argv[a++] = app_file("cacert.pem"); argv[a++] = "-tls_verify"; argv[a++] = "1";
            argv[a++] = "-user_agent"; argv[a++] = USER_AGENT;
            argv[a++] = "-reconnect"; argv[a++] = "1"; argv[a++] = "-reconnect_delay_max"; argv[a++] = "4";
            argv[a++] = "-fflags"; argv[a++] = "+fastseek";
            if (seek_ms > 500) { argv[a++] = "-ss"; argv[a++] = ss; }
            argv[a++] = "-i"; argv[a++] = eff;
        }
        argv[a++] = "-vn"; argv[a++] = "-ac"; argv[a++] = "2";
        /* integer resampler: the default float path is software-emulated on this
         * FPU-less CPU and runs >2x slower than real time for 48 kHz podcasts */
        argv[a++] = "-af"; argv[a++] = "aresample=44100:internal_sample_fmt=s16p";
        argv[a++] = "-f"; argv[a++] = "s16le"; argv[a++] = "-";
        argv[a] = NULL;
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    int used_nb = 0;
    if (direct) {
        close(in[0]);
        if (nbi >= 0 && prebuf_attach(nb[nbi].p, from, in[1]) == 0) { nb[nbi].p = NULL; used_nb = 1; }
        else if (fetch_start(eff, from, in[1])) { LOG("audio: fetch thread failed"); }
    }
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->pid = pid;
    s->fd = p[0];
    set_nonblock(s->fd);
    s->st = st; s->slot = slot;
    snprintf(s->url, sizeof s->url, "%s", url);
    s->start_ms = seek_ms > 500 ? seek_ms : 0;
    s->ring = malloc(RING_BYTES);
    s->launched_ms = now_ms();
    s->tune_launch = seek_ms > 500;
    s->direct = used_nb ? 2 : direct;
    LOG("audio: launch st=%d slot=%d seek=%lld ms pid=%d %s%.60s", st, slot, (long long)seek_ms, pid,
        used_nb ? "(pre-buffered) " : direct ? "(direct mp3 @byte) " : strcmp(eff, url) ? "(resolved) " : "", eff);
    if (direct) LOG("audio: byte %lld of %lld (%d kbps%s)", (long long)from, (long long)mi.total, mi.bitrate_kbps, mi.has_toc ? ", VBR toc" : "");
    return s;
}

void audio_tune(int st) {
    if (st == tuned) return;
    if (st >= 0 && tuned >= 0 && st != outgoing) {   /* keep the old station audible */
        outgoing = tuned; outgoing_since = now_ms(); xfade_start = 0;
    } else outgoing = -1;
    tuned = st;
    tuned_at = now_ms();
    state = st < 0 ? AUDIO_IDLE : AUDIO_TUNING;
    have_pending = 0;   /* drop the queued chunk from the old station */
}

void audio_set_muted(int m) { muted = m; }

/* Master volume as a gain in the mixer: 3 dB per step below `steps`; 0 = silent. */
static int32_t master_q15 = 32767;
void audio_set_volume(int v, int steps) {
    if (v <= 0) { master_q15 = 0; return; }
    if (v > steps) v = steps;
    master_q15 = (int32_t)(32767.0 * pow(10.0, -3.0 * (steps - v) / 20.0));
}
void audio_set_network_ready(int r) { net_ready = r; }
void audio_set_neighbors(int a, int b) { nb_want[0] = a; nb_want[1] = b; }

/* Keep the neighbor pre-buffers on their stations' live MP3 episodes. */
static void nb_service(const sched_t *sc) {
    if (!sc || !net_ready) return;
    int64_t now = now_ms();
    for (int i = 0; i < NB_N; i++) {
        int st = nb_want[i];
        int slot = -1; int64_t off = 0;
        const ep_t *ep = NULL;
        if (st >= 0 && st < sc->n && st != tuned && !sched_live(sc, st, now, &slot, &off)) ep = &sc->st[st].eps[slot];
        char eff[2048]; probe_t pr;
        int ok = ep && (int64_t)ep->dur_s * 1000 - off > 30000 && !ep->bulletin &&
                 resolver_lookup_probe(ep->url, eff, sizeof eff, &pr) && (pr.fmt == FMT_MP3 || pr.fmt == FMT_MP4);
        int have = ok && nb[i].p && nb[i].st == st && nb[i].slot == slot && !strcmp(nb[i].url, ep->url);
        if (!have && nb[i].p) { prebuf_cancel(nb[i].p); nb[i].p = NULL; }
        if (!ok) continue;
        int64_t limit = est_byte(&pr, off + 15000);
        if (have) { prebuf_set_limit(nb[i].p, limit); continue; }
        /* a new pre-buffer costs a TLS handshake: never while a tune is starting */
        if (state != AUDIO_PLAYING) continue;
        int64_t start = est_byte(&pr, off > 6000 ? off - 6000 : 0);   /* MP4 estimate: a wider margin */
        nb[i].p = prebuf_start(eff, start, limit, NB_CAP);
        nb[i].st = st; nb[i].slot = slot; nb[i].pr = pr;
        snprintf(nb[i].url, sizeof nb[i].url, "%s", ep->url);
        LOG("audio: pre-buffering st=%d slot=%d from byte %lld", st, slot, (long long)start);
    }
}

void audio_schedule_changed(void) { /* streams are matched by url each tick; nothing to do */ }

int audio_pollfds(struct pollfd *fds, int max) {
    int n = 0;
    if (aplay_fd >= 0 && n < max) { fds[n].fd = aplay_fd; fds[n].events = POLLOUT; n++; }
    for (int i = 0; i < MAX_STREAMS && n < max; i++)
        if (streams[i].used && !streams[i].eof && streams[i].rlen < RING_BYTES) {
            fds[n].fd = streams[i].fd; fds[n].events = POLLIN; n++;
        }
    return n;
}

static void pump(stream_t *s) {
    while (!s->eof && s->rlen < RING_BYTES) {
        int tail = (s->rhead + s->rlen) % RING_BYTES;
        int space = RING_BYTES - s->rlen;
        int contig = RING_BYTES - tail;
        int want = space < contig ? space : contig;
        ssize_t r = read(s->fd, s->ring + tail, (size_t)want);
        if (r > 0) {
            if (!s->first_pcm_ms) {
                s->first_pcm_ms = now_ms();
                /* adapt the start-up lead to what we observe; a start that took
                 * forever (network still coming up) says nothing about the
                 * next tune, and over-leading means silent waiting */
                int64_t took = s->first_pcm_ms - s->launched_ms + 600;
                int d = s->direct;
                if (s->tune_launch && took < lead_outlier[d]) {
                    lead_ms[d] = (3 * lead_ms[d] + took) / 4;
                    if (lead_ms[d] < lead_min[d]) lead_ms[d] = lead_min[d];
                    if (lead_ms[d] > lead_max[d]) lead_ms[d] = lead_max[d];
                }
                LOG("audio: first PCM st=%d slot=%d after %lld ms (%s lead now %lld)", s->st, s->slot,
                    (long long)(s->first_pcm_ms - s->launched_ms), (const char *[]){"ffmpeg", "native", "pre-buffered", "native aac"}[d], (long long)lead_ms[d]);
            }
            s->rlen += (int)r;
            s->got_bytes += r;
        } else if (r == 0) { s->eof = 1; }
        else break;   /* EAGAIN */
    }
}

static int64_t stream_pos_ms(const stream_t *s) { return s->start_ms + s->frames * 1000 / RATE; }

/* Copy (dst != NULL) or drop `frames` frames from the ring; returns frames done. */
static int take(stream_t *s, int16_t *dst, int frames) {
    int avail = s->rlen / FRAME;
    if (frames > avail) frames = avail;
    int bytes = frames * FRAME, done = 0;
    while (done < bytes) {
        int contig = RING_BYTES - s->rhead;
        int n = bytes - done < contig ? bytes - done : contig;
        if (dst) memcpy((uint8_t *)dst + done, s->ring + s->rhead, (size_t)n);
        s->rhead = (s->rhead + n) % RING_BYTES;
        s->rlen -= n;
        done += n;
    }
    s->frames += frames;
    return frames;
}

static int32_t gain_q15(double gain_db) {
    if (gain_db > 12) gain_db = 12;
    if (gain_db < -12) gain_db = -12;
    double v = 0.5 * pow(10.0, gain_db / 20.0);   /* phone parity: 0.5 operating point */
    if (v > 1.0) v = 1.0;
    return (int32_t)(v * 32767);
}

/* Build the next output chunk (always CHUNK_FRAMES, silence where needed). */
/* Render one station's next chunk at its live position into out (silence where
 * there's nothing). `primary`: the tuned station — may launch decoders and
 * drives the UI state. Returns frames of real audio written. */
static int produce_station(const sched_t *sc, int sti, int16_t *out, int primary) {
    int dummy;
    int *stp = primary ? &state : &dummy;
#define state (*stp)
    memset(out, 0, CHUNK_FRAMES * FRAME);
    if (sti < 0 || !sc || sti >= sc->n) { state = AUDIO_IDLE; return 0; }
    int64_t t = now_ms() + OUT_LATENCY_MS;   /* when this chunk will be heard */
    int slot; int64_t off;
    if (sched_live(sc, sti, t, &slot, &off)) return 0;
    const station_t *st = &sc->st[sti];
    const ep_t *ep = &st->eps[slot];

    /* pre-launch the next slot so it starts exactly at its boundary */
    int64_t remaining = (int64_t)ep->dur_s * 1000 + GAP_MS - off;
    int nslot = (slot + 1) % st->n;
    if (primary && remaining < PRELAUNCH_MS && !find_stream(sti, nslot, st->eps[nslot].url))
        launch(sti, nslot, st->eps[nslot].url, 0);

    stream_t *s = find_stream(sti, slot, ep->url);
    if (!s) {
        int64_t audio_end = ep->bulletin && ep->dur_s * 1000 > BULLETIN_CAP_MS ? BULLETIN_CAP_MS : (int64_t)ep->dur_s * 1000;
        char tmp[2048]; mp3info_t mi;
        probe_t pr;
        (void)mi;
        int path = nb_find(sti, slot, ep->url) >= 0 ? 2 : !resolver_lookup_probe(ep->url, tmp, sizeof tmp, &pr) ? 0 : pr.fmt == FMT_MP4 ? 3 : 1;
        int64_t lead = lead_ms[path];
        /* not resolved yet (new episode, or re-resolving an expired link): give the
         * resolver ~10 s rather than dropping to ffmpeg's 15-20 s path; the old
         * station keeps playing through the crossfade meanwhile */
        int rs = resolver_status(ep->url);
        int wait = rs == 0 && now_ms() - tuned_at < 10000;
        if (primary && net_ready && !wait && off + lead < audio_end - 1500 && now_ms() - fail_at[sti] > 3000)
            launch(sti, slot, ep->url, off + lead);
        if (state != AUDIO_GAP) state = AUDIO_TUNING;
        return 0;
    }
    int64_t audio_end = (int64_t)ep->dur_s * 1000;
    if (ep->bulletin && audio_end > BULLETIN_CAP_MS) audio_end = BULLETIN_CAP_MS;
    if (off >= audio_end) { state = AUDIO_GAP; return 0; }   /* trailing gap / bulletin cap */

    int64_t diff = off - stream_pos_ms(s);
    if (diff > SYNC_SLOP_MS) {                 /* behind live: drop to catch up */
        int64_t drop = diff * RATE / 1000;
        if (drop > INT32_MAX) drop = INT32_MAX;
        take(s, NULL, (int)drop);
        diff = off - stream_pos_ms(s);
        if (diff > SYNC_SLOP_MS) { state = s->first_pcm_ms ? AUDIO_PLAYING : AUDIO_TUNING; return 0; }
    }
    if (diff < -SYNC_SLOP_MS) { state = AUDIO_TUNING; return 0; }   /* early: hold until live */

    int got = take(s, out, CHUNK_FRAMES);
    if (got == 0) { state = s->eof ? AUDIO_GAP : AUDIO_TUNING; return 0; }
    state = AUDIO_PLAYING;
    int32_t g = muted ? 0 : (int32_t)(((int64_t)gain_q15(ep->gain_db) * master_q15) >> 15);
    for (int i = 0; i < got; i++) {
        int32_t gi = g;
        if (s->fade < FADE_FRAMES) { gi = (int32_t)((int64_t)g * s->fade / FADE_FRAMES); s->fade++; }
        out[2 * i] = (int16_t)((out[2 * i] * gi) >> 15);
        out[2 * i + 1] = (int16_t)((out[2 * i + 1] * gi) >> 15);
    }
    return got;
#undef state
}

static int16_t buf_old[CHUNK_FRAMES * 2];

static void produce(const sched_t *sc, int16_t *out) {
    int got_new = produce_station(sc, tuned, out, 1);
    if (outgoing < 0) return;
    int64_t now = now_ms();
    if (!sc || outgoing >= sc->n || (!xfade_start && now - outgoing_since > OUTGOING_MAX_MS)) { outgoing = -1; return; }
    produce_station(sc, outgoing, buf_old, 0);
    if (!xfade_start) {
        if (!got_new) { memcpy(out, buf_old, sizeof buf_old); return; }   /* old station until the new has audio */
        xfade_start = now;
    }
    double t = (double)(now - xfade_start) / XFADE_MS;
    if (t >= 1.0) { outgoing = -1; return; }
    int32_t gn = (int32_t)(sin(t * M_PI / 2) * 32767), go = (int32_t)(cos(t * M_PI / 2) * 32767);
    for (int i = 0; i < CHUNK_FRAMES * 2; i++) {
        int32_t v = (out[i] * gn + buf_old[i] * go) >> 15;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void audio_service(const sched_t *sc) {
    /* reap exited children; respawn aplay if it died */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == aplay_pid) {
            LOG("audio: aplay exited (%d), respawning", status);
            close(aplay_fd); aplay_fd = -1; aplay_pid = -1;
            spawn_aplay();
        }
        for (int i = 0; i < MAX_STREAMS; i++) if (streams[i].pid == pid) streams[i].pid = -1;
    }

    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i].used) pump(&streams[i]);
    static int64_t nb_last;
    if (now_ms() - nb_last > 1000) { nb_service(sc); nb_last = now_ms(); }

    /* keep only the tuned station's current and next slot */
    int cur = -1, nxt = -1;
    if (sc && tuned >= 0 && tuned < sc->n) {
        int64_t off;
        if (!sched_live(sc, tuned, now_ms() + OUT_LATENCY_MS, &cur, &off)) nxt = (cur + 1) % sc->st[tuned].n;
    }
    int ocur = -1;
    if (sc && outgoing >= 0 && outgoing < sc->n) {
        int64_t off;
        if (sched_live(sc, outgoing, now_ms() + OUT_LATENCY_MS, &ocur, &off)) ocur = -1;
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &streams[i];
        if (!s->used) continue;
        int keep = sc && s->st == tuned && tuned < sc->n &&
                   ((s->slot == cur && !strcmp(s->url, sc->st[tuned].eps[cur].url)) ||
                    (s->slot == nxt && !strcmp(s->url, sc->st[tuned].eps[nxt].url)));
        if (!keep && sc && s->st == outgoing && ocur >= 0 && s->slot == ocur &&
            !strcmp(s->url, sc->st[outgoing].eps[ocur].url))
            keep = 1;   /* the old station, until the crossfade completes */
        if (keep && s->eof && s->got_bytes == 0) {   /* decoder failed outright */
            LOG("audio: decoder for st=%d slot=%d produced nothing; re-resolving", s->st, s->slot);
            resolver_invalidate(s->url);
            fail_at[s->st] = now_ms();
            tuned_at = now_ms();   /* give the re-resolve its ~10 s before any fallback */
            keep = 0;
        }
        if (!keep) stream_kill(s);
    }

    /* write as many chunks as the output pipe accepts */
    for (int n = 0; n < 24 && aplay_fd >= 0; n++) {
        if (!have_pending) { produce(sc, pending); have_pending = 1; }
        ssize_t w = write(aplay_fd, pending, sizeof pending);
        if (w == (ssize_t)sizeof pending) { have_pending = 0; continue; }
        break;   /* EAGAIN (pipe full) */
    }
}

int audio_state(void) { return state; }

/* Close the device gently: this 2.6.28 sound driver can wedge for good (aplay
 * stuck in snd_pcm_open, no audio until a reboot) if a new aplay opens it while
 * the last one is still being torn down. So: EOF, let aplay drain and close, and
 * wait for it; SIGTERM only as a fallback. boot.sh also waits for every aplay
 * (the dmix server included) to be gone before starting the next client. */
void audio_shutdown(void) {
    for (int i = 0; i < MAX_STREAMS; i++) stream_kill(&streams[i]);
    if (aplay_fd >= 0) { close(aplay_fd); aplay_fd = -1; }
    if (aplay_pid <= 0) return;
    for (int i = 0; i < 30; i++) {                  /* up to 1.5 s to drain and exit */
        if (waitpid(aplay_pid, NULL, WNOHANG) == aplay_pid) { aplay_pid = -1; return; }
        usleep(50000);
    }
    kill(aplay_pid, SIGTERM);
    for (int i = 0; i < 20 && waitpid(aplay_pid, NULL, WNOHANG) != aplay_pid; i++) usleep(50000);
    aplay_pid = -1;
}
