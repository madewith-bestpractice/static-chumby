#include "common.h"
#include "decode.h"
#include "mp4.h"
#include "resample.h"
#include "third_party/helix/libhelix-mp3/mp3dec.h"
#include "third_party/helix/libhelix-aac/aacdec.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    char url[2048];
    probe_t pr;
    int64_t seek_ms;
    int in_fd, out_fd;
    int64_t start_byte;
    mp4idx_t mp4;
    uint32_t mp4_first;
    int16_t *pcm;      /* decoder output */
    int16_t *out;      /* resampler output */
} job_t;

#define PCM_FRAMES 2048      /* max frames per decode call (AAC+SBR) */
#define OUT_FRAMES 4096

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w; n -= (size_t)w;
    }
    return 0;
}

/* Input buffer over the compressed byte stream; tracks the absolute position. */
typedef struct { int fd; uint8_t buf[32768]; int len, off; int64_t pos; int eof; } in_t;

static int fill(in_t *in) {   /* compact, then read more; 0 on EOF/error */
    if (in->off) { memmove(in->buf, in->buf + in->off, (size_t)(in->len - in->off)); in->len -= in->off; in->off = 0; }
    if (in->len == (int)sizeof in->buf) return 1;
    ssize_t r;
    do r = read(in->fd, in->buf + in->len, sizeof in->buf - (size_t)in->len); while (r < 0 && errno == EINTR);
    if (r <= 0) { in->eof = 1; return 0; }
    in->len += (int)r;
    return 1;
}

/* frames <= PCM_FRAMES, so the output (<= frames * 44100 / 8000 + 8) always fits */
static int emit(job_t *j, rs_t **rs, int rate, const int16_t *pcm, int frames, int ch) {
    if (!*rs) *rs = rs_new(rate);
    int n = rs_push(*rs, pcm, frames, ch, j->out, OUT_FRAMES);
    return n ? write_all(j->out_fd, j->out, (size_t)n * 4) : 0;
}

static void run_mp3(job_t *j) {
    HMP3Decoder d = MP3InitDecoder();
    if (!d) return;
    in_t *in = calloc(1, sizeof *in);
    in->fd = j->in_fd;
    rs_t *rs = NULL;
    int16_t *pcm = j->pcm;
    for (;;) {
        if (in->len - in->off < 4096 && !in->eof) fill(in);
        int avail = in->len - in->off;
        if (avail <= 0) break;
        int s = MP3FindSyncWord(in->buf + in->off, avail);
        if (s < 0) { in->off = in->len - 3 > in->off ? in->len - 3 : in->off; if (in->eof) break; fill(in); continue; }
        in->off += s;
        unsigned char *p = in->buf + in->off;
        int left = in->len - in->off;
        int err = MP3Decode(d, &p, &left, pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) { if (in->eof) break; if (!fill(in) && in->len - in->off == avail) break; continue; }
        in->off = (int)(p - in->buf);
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) continue;    /* bit reservoir warming up */
        if (err) { in->off++; continue; }                    /* resync */
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0) continue;
        if (emit(j, &rs, fi.samprate, pcm, fi.outputSamps / fi.nChans, fi.nChans)) break;
    }
    rs_free(rs);
    free(in);
    MP3FreeDecoder(d);
}

static void run_aac_mp4(job_t *j) {
    HAACDecoder d = AACInitDecoder();
    if (!d) return;
    AACFrameInfo fi = {0};
    fi.nChans = j->mp4.channels;
    fi.sampRateCore = j->mp4.core_rate;
    fi.profile = AAC_PROFILE_LC;
    AACSetRawBlockParams(d, 0, &fi);
    in_t *in = calloc(1, sizeof *in);
    in->fd = j->in_fd;
    in->pos = j->start_byte;   /* absolute byte of buf[0] */
    rs_t *rs = NULL;
    int16_t *pcm = j->pcm;
    mp4iter_t it;
    if (mp4_iter_seek(&j->mp4, j->mp4_first, &it)) goto out;
    uint64_t off; uint32_t size;
    while (mp4_iter_next(&j->mp4, &it, &off, &size)) {
        if (size > sizeof in->buf / 2) break;
        /* make [off, off+size) available in the buffer */
        for (;;) {
            int64_t have_lo = in->pos + in->off, have_hi = in->pos + in->len;
            if ((int64_t)off < have_lo) goto out;                 /* went backwards: not seekable */
            if ((int64_t)off >= have_hi) {                        /* skip a gap between chunks */
                in->pos += in->len; in->len = in->off = 0;
                if (!fill(in)) goto out;
                continue;
            }
            in->off = (int)((int64_t)off - in->pos);
            if ((int64_t)off + size <= have_hi) break;
            int before = in->off;
            if (!fill(in)) goto out;
            in->pos += before;                                    /* fill() compacted off -> 0 */
        }
        unsigned char *p = in->buf + in->off;
        int left = (int)size;
        int err = AACDecode(d, &p, &left, pcm);
        in->off += (int)size;
        if (err) continue;
        AACGetLastFrameInfo(d, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0) continue;
        if (emit(j, &rs, fi.sampRateOut, pcm, fi.outputSamps / fi.nChans, fi.nChans)) break;
    }
out:
    rs_free(rs);
    free(in);
    AACFreeDecoder(d);
}

static void run_wav(job_t *j) {
    in_t *in = calloc(1, sizeof *in);
    in->fd = j->in_fd;
    rs_t *rs = NULL;
    int frame = 2 * j->pr.wav_ch;
    for (;;) {
        if (!fill(in) && in->len == 0) break;
        int frames = in->len / frame;
        if (!frames) { if (in->eof) break; continue; }
        if (frames > PCM_FRAMES) frames = PCM_FRAMES;
        if (emit(j, &rs, j->pr.wav_rate, (const int16_t *)in->buf, frames, j->pr.wav_ch)) break;
        in->off = frames * frame;
    }
    rs_free(rs);
    free(in);
}

static uint8_t *load_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static void *run(void *arg) {
    job_t *j = arg;
    j->pcm = malloc(PCM_FRAMES * 2 * sizeof(int16_t));
    j->out = malloc(OUT_FRAMES * 2 * sizeof(int16_t));
    if (!j->pcm || !j->out) {}
    else if (j->pr.fmt == FMT_MP3) run_mp3(j);
    else if (j->pr.fmt == FMT_MP4) run_aac_mp4(j);
    else if (j->pr.fmt == FMT_WAV) run_wav(j);
    close(j->in_fd);    /* the feeder sees EPIPE and stops */
    close(j->out_fd);   /* the audio engine sees EOF */
    if (j->pr.fmt == FMT_MP4) mp4_free(&j->mp4);
    free(j->pcm); free(j->out);
    free(j);
    return NULL;
}

int decode_start(const char *url, const probe_t *probe, int64_t seek_ms, prebuf_t *pb, int out_fd, int *used_pb) {
    *used_pb = 0;
    job_t *j = calloc(1, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", url);
    j->pr = *probe;
    j->seek_ms = seek_ms;
    j->out_fd = out_fd;
    /* where to start reading */
    if (probe->fmt == FMT_MP3) j->start_byte = seek_ms > 500 ? mp3_byte_for_ms(&probe->mp3, seek_ms) : 0;
    else if (probe->fmt == FMT_WAV) {
        int64_t fb = 2LL * probe->wav_ch;
        j->start_byte = probe->wav_data + (seek_ms * probe->wav_rate / 1000) * fb;
    } else if (probe->fmt == FMT_MP4) {
        size_t len = 0;
        uint8_t *moov = load_file(probe->moov_path, &len);
        int ok = moov && mp4_parse_moov(moov, len, &j->mp4) == 0;
        free(moov);
        mp4iter_t it;
        if (!ok) { free(j); return -1; }
        j->mp4_first = mp4_sample_for_ms(&j->mp4, seek_ms, NULL);
        if (mp4_iter_seek(&j->mp4, j->mp4_first, &it)) { mp4_free(&j->mp4); free(j); return -1; }
        j->start_byte = (int64_t)it.off;
    } else { free(j); return -1; }

    int in[2];
    if (pipe(in)) { if (probe->fmt == FMT_MP4) mp4_free(&j->mp4); free(j); return -1; }
    fcntl(in[0], F_SETFD, FD_CLOEXEC);
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    j->in_fd = in[0];
    if (pb && prebuf_attach(pb, j->start_byte, in[1]) == 0) *used_pb = 1;
    else {
        if (pb) {   /* a miss falls through to a normal fetch (it used to start nothing: a silent stall) */
            int64_t lo, hi;
            prebuf_window(pb, &lo, &hi);
            LOG("decode: pre-buffer missed: need byte %lld, window [%lld, %lld) (%+lld KB from its start)",
                (long long)j->start_byte, (long long)lo, (long long)hi, (long long)((j->start_byte - lo) / 1024));
        }
        if (fetch_start(url, j->start_byte, in[1])) { close(in[0]); close(in[1]); if (probe->fmt == FMT_MP4) mp4_free(&j->mp4); free(j); return -1; }
    }

    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, run, j)) { close(in[0]); if (probe->fmt == FMT_MP4) mp4_free(&j->mp4); free(j); return -1; }
    return 0;
}
