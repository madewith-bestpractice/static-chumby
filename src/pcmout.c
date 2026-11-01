/* Sound output straight to the kernel's ALSA PCM device: no aplay process, no
 * alsa-lib, no dmix server. A writer thread reads the mixer's pipe and pushes
 * it to /dev/snd/pcmC0D0p with the raw ioctls (as tinyalsa does). This saves
 * the aplay process (~6% CPU here) and removes the dmix server whose teardown
 * race could wedge this 2.6.28 driver (aplay stuck in snd_pcm_open: no audio
 * until a reboot).
 *
 * Same shape as before: 44.1 kHz s16 stereo, 2048-frame periods, a 16384-frame
 * (371 ms) buffer, the settings dmix used on this device. */
#include "common.h"
#include "pcmout.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PCM_DEV "/dev/snd/pcmC0D0p"
#define PERIOD 2048
#define PERIODS 8

static int pcm_fd = -1, in_fd = -1;
static volatile int running, done;

static void p_mask(struct snd_pcm_hw_params *p, int n, unsigned bit) {
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void p_int(struct snd_pcm_hw_params *p, int n, unsigned v) {
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->integer = 1;
}

static void p_init(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof *p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        m->bits[0] = m->bits[1] = ~0u;
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        i->min = 0;
        i->max = ~0u;
    }
    p->rmask = ~0u;
    p->cmask = 0;
    p->info = ~0u;
}

static int pcm_setup(void) {
    /* O_NONBLOCK so a busy device (an old aplay/dmix still holding it) fails
     * fast instead of blocking; writes are blocking afterwards. */
    int fd = open(PCM_DEV, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { LOG("pcm: open %s: %s", PCM_DEV, strerror(errno)); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    struct snd_pcm_hw_params hw;
    p_init(&hw);
    p_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    p_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    p_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    p_int(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    p_int(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    p_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    p_int(&hw, SNDRV_PCM_HW_PARAM_RATE, 44100);
    p_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    p_int(&hw, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw)) {
        LOG("pcm: hw_params: %s", strerror(errno));
        close(fd);
        return -1;
    }
    unsigned buffer = PERIOD * PERIODS;

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = PERIOD;
    sw.start_threshold = buffer / 2;     /* start once ~185 ms is queued */
    sw.stop_threshold = buffer;          /* stop (and report) on underrun */
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    unsigned long boundary = buffer;
    while (boundary * 2 <= 0x7fffffffUL - buffer) boundary *= 2;
    sw.boundary = boundary;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw)) {
        LOG("pcm: sw_params: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE)) {
        LOG("pcm: prepare: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int write_frames(const int16_t *buf, unsigned frames) {
    while (frames) {
        struct snd_xferi x = { .result = 0, .buf = (void *)buf, .frames = frames };
        if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) {   /* underrun (the main loop stalled): restart */
                LOG("pcm: underrun");
                if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE)) return -1;
                continue;
            }
            LOG("pcm: write: %s", strerror(errno));
            return -1;
        }
        buf += x.result * 2;
        frames -= (unsigned)x.result;
    }
    return 0;
}

static void *run(void *arg) {
    (void)arg;
    static int16_t buf[4096 * 2];
    int have = 0;   /* bytes in buf */
    for (;;) {
        ssize_t r = read(in_fd, (char *)buf + have, sizeof buf - (size_t)have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;   /* EOF: the client is shutting down */
        have += (int)r;
        unsigned frames = (unsigned)have / 4;
        if (frames && write_frames(buf, frames)) break;
        int used = (int)frames * 4;
        memmove(buf, (char *)buf + used, (size_t)(have - used));
        have -= used;
    }
    ioctl(pcm_fd, SNDRV_PCM_IOCTL_DRAIN);
    close(pcm_fd);
    close(in_fd);
    pcm_fd = in_fd = -1;
    running = 0;
    done = 1;
    return NULL;
}

int pcmout_start(int read_fd) {
    if (running) return -1;
    if (access(STATIC_DIR "/use-aplay", F_OK) == 0) return -1;   /* escape hatch */
    int fd = pcm_setup();
    if (fd < 0) return -1;
    pcm_fd = fd;
    in_fd = read_fd;
    running = 1;
    done = 0;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, run, NULL)) { close(fd); pcm_fd = -1; running = 0; return -1; }
    LOG("pcm: direct output on %s (%d x %d frames)", PCM_DEV, PERIODS, PERIOD);
    return 0;
}

int pcmout_running(void) { return running; }
int pcmout_done(void) { return done; }
