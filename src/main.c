/* Static radio for the Chumby One.
 *
 * Knob = the dial: one notch per station, tunes once it rests. The riso art
 * fills the left square with the phone's icons over it (listener face + count,
 * heart, info, faves list); the stations run down the right beside the knob.
 * Bottom-left: the time (the chumby's own time zone and 12/24 h setting) and
 * the volume glyph (tap: mute). Hold the top button and turn: volume. Hold it
 * still for a moment: the system menu (chumby settings, install, updates). */
#include "common.h"
#include <stdint.h>
#include "audio.h"
#include "fonts.h"
#include "icons.h"
#include "resolver.h"
#include "sched.h"
#include "splash.h"
#include "update.h"
#include "third_party/qrcodegen.h"
#include "ui.h"
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <psa/crypto.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define STEPS_PER_STATION 1          /* one notch = one station; the list slide + art crossfade carry the motion */
#define SETTLE_MS 300                 /* tune once the knob rests this long */
#define LAST_STATION_FILE STATIC_DIR "/last_station"
#define VOLUME_FILE STATIC_DIR "/volume"
#define FAVES_FILE STATIC_DIR "/faves.tsv"
#define VOL_STEPS 10
#define MAX_FAVES 512
/* Touch calibration from /psp/ts_settings (136,3768,3835,-3540), verified by
 * corner taps: (300,10) top-right, (10,220) bottom-left. */
#define TS_X0 136
#define TS_XSPAN 3768
#define TS_Y0 3835
#define TS_YSPAN (-3540)
#define TAP_DEBOUNCE_MS 250
#define PAPER 0xF79D                  /* #F5F3EF */

/* Layout (320x240): the riso art is a 240x240 square at left with the app icons
 * over it; the station labels run down the right beside the knob, with a needle
 * that slides up and down as you turn. */
#define ART_W 240
#define COL_X 240                     /* station column */
#define COL_W (W - COL_X)
#define ICON_Y 8                      /* icon row over the top of the art */
/* Top-right constellation, like the phone: heart, info dot, and the faves list
 * under the info dot (rotated 180, as on the phone) with the dotted trail from
 * the heart down and across into it while faves exist. */
#define ICON_HEART_X 170
#define ICON_INFO_X 206
#define ICON_LIST_X 206
#define ICON_LIST_Y 40
#define TRAIL_MS 700
#define VOL_Y 206                     /* bottom bar: time, then volume to its right */
#define LABEL_ROW 27                  /* station column row height */

/* staticradio.app palette (assets/site.css) */
#define SITE_INK  0x10C3              /* #17181A */
#define SITE_DARK 0x2965              /* #2C2C2E */
#define SITE_TEAL 0x67FD              /* #62FFED */
#define SITE_PALE 0xDFDE              /* #DAF8F6 */
#define QUIET_INK 0x8D13              /* pale at ~65%: time + volume sit back behind the icons */

enum { MODE_PLAYER, MODE_FAVES, MODE_INFO, MODE_FAVE_INFO, MODE_QR, MODE_SYSTEM };
static int qr_back = 0;                     /* mode to return to from the big QR */
static char qr_url[1024];
/* "Episode page" button on the detail views (tap: the QR, big) */
#define QR_BTN_X (W - 138)
#define QR_BTN_Y 192
#define QR_BTN_W 128
#define QR_BTN_H 32


static sched_t *sched;
static int knob_fd = -1, touch_fd = -1, bend_fd = -1;
static int steps;                     /* knob position in detents */
static double dial;                   /* displayed dial position (stations, eased) */
static int64_t last_knob_ms;
static int last_dir = 1;              /* direction of the last detent */
static int tuned = -1, muted, mode = MODE_PLAYER;
static char last_id[24];
static int volume = 7;
static int64_t volume_shown_ms;      /* -/+ revealed until this + VOL_SHOW_MS */
static int touch_x = -1, touch_y = -1, touch_down;
static int64_t last_tap_ms;
/* Volume = hold the top button and turn the knob; mute = the on-screen volume
 * glyph. */
static int bend_down, bend_turned, vol_overlay_was;
static int64_t bend_down_ms;
static int hold_opened;                /* this press opened the system menu */
#define HOLD_MENU_MS 1200              /* hold the top button still this long: system menu */
static int sys_confirm;                /* "turn off" armed: tap again to confirm */
static int64_t sys_confirm_ms;
static int clock24;                    /* /psp/clock_format */
static int64_t offline_since;          /* no live schedule yet, since */
#define OFFLINE_HINT_MS 20000
#define VOL_OVERLAY_MS 1200            /* the level overlay lingers after release */
static int faves_scroll;
static int64_t trail_changed_ms = -100000;   /* when the trail last started revealing/retracting */
static int trail_on;                         /* revealed (faves exist) */
static uint16_t art_shown_raw[ART_W * H];    /* the last real art (held until the next arrives) */
static int art_have;
static const void *art_shown_id;             /* identity of what's in art_shown */
static volatile sig_atomic_t quit;

static int mod(int a, int n) { return ((a % n) + n) % n; }

/* ---------- faves: on-device, key = feedUrl ?? audioUrl (phone parity) ---------- */

/* faves.tsv: key \t show \t title \t unix time \t episode page \t image url */
typedef struct { char *key, *show, *title; long ts; char *link, *image; } fave_t;
static int fave_sel = -1;             /* fave shown in MODE_FAVE_INFO */
static fave_t faves[MAX_FAVES];
static int nfaves;

static const char *fave_key(const ep_t *ep) { return ep->feed && ep->feed[0] ? ep->feed : ep->url; }

static int fave_index(const char *key) {
    for (int i = 0; i < nfaves; i++) if (!strcmp(faves[i].key, key)) return i;
    return -1;
}

static void load_faves(void) {
    FILE *f = fopen(FAVES_FILE, "r");
    if (!f) return;
    char line[4096];
    while (nfaves < MAX_FAVES && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *rest = line, *f[6] = {0};
        for (int i = 0; i < 6; i++) f[i] = strsep(&rest, "\t");   /* strsep keeps empty fields */
        if (!f[0] || !*f[0]) continue;
        faves[nfaves++] = (fave_t){ strdup(f[0]), strdup(f[1] ? f[1] : ""), strdup(f[2] ? f[2] : ""),
                                    f[3] ? atol(f[3]) : 0, strdup(f[4] ? f[4] : ""), strdup(f[5] ? f[5] : "") };
    }
    fclose(f);
}

static void save_faves(void) {
    FILE *f = fopen(FAVES_FILE ".new", "w");
    if (!f) return;
    for (int i = 0; i < nfaves; i++)
        fprintf(f, "%s\t%s\t%s\t%ld\t%s\t%s\n", faves[i].key, faves[i].show, faves[i].title, faves[i].ts,
                faves[i].link, faves[i].image);
    fclose(f);
    rename(FAVES_FILE ".new", FAVES_FILE);
}

static const ep_t *find_ep_by_key(const char *key);

/* Older faves were saved before page/image URLs: fill them in from the current
 * schedule's episode of the same show, when one is on air somewhere. */
static void backfill_fave(fave_t *f) {
    if ((f->link[0] && f->image[0]) || !sched) return;
    const ep_t *ep = find_ep_by_key(f->key);
    if (!ep) return;
    if (!f->link[0] && ep->link && ep->link[0]) { free(f->link); f->link = strdup(ep->link); }
    if (!f->image[0] && ep->image && ep->image[0]) { free(f->image); f->image = strdup(ep->image); }
    save_faves();
}

static void strip_tabs(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static void toggle_fave(const ep_t *ep) {
    const char *key = fave_key(ep);
    if (!key[0]) return;
    int i = fave_index(key);
    if (i >= 0) {
        free(faves[i].key); free(faves[i].show); free(faves[i].title); free(faves[i].link); free(faves[i].image);
        memmove(&faves[i], &faves[i + 1], (size_t)(nfaves - i - 1) * sizeof faves[0]);
        nfaves--;
    } else if (nfaves < MAX_FAVES) {
        fave_t f = { strdup(key), strdup(ep->show), strdup(ep->title), (long)time(NULL),
                     strdup(ep->link ? ep->link : ""), strdup(ep->image ? ep->image : "") };
        strip_tabs(f.show); strip_tabs(f.title);
        faves[nfaves++] = f;
    }
    save_faves();
    if ((nfaves > 0) != trail_on) { trail_on = nfaves > 0; trail_changed_ms = now_ms(); }
    LOG("fave %s: %s", i >= 0 ? "removed" : "added", ep->show);
}

/* ---------- persisted settings ---------- */

static void load_last_station(void) {
    FILE *f = fopen(LAST_STATION_FILE, "r");
    if (!f) return;
    if (fgets(last_id, sizeof last_id, f)) last_id[strcspn(last_id, "\r\n")] = 0;
    fclose(f);
}

static void save_last_station(const char *id) {
    FILE *f = fopen(LAST_STATION_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", id);
    fclose(f);
}

/* DAC is 0..255; the stock UI had left it at 0. Steps map onto the audible top
 * of the range; 0 = silent. */
/* Volume is a gain in our own mixer (3 dB per step; 0 = silent). Changing the
 * DAC mid-playback made the codec driver mute/click on every step, so the DAC
 * is set to full once at start-up and left alone. */
static void apply_volume(void) {
    audio_set_volume(volume, VOL_STEPS);
    FILE *f = fopen(VOLUME_FILE, "w");
    if (f) { fprintf(f, "%d\n", volume); fclose(f); }
}

static void dac_full_once(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/amixer", "amixer", "-q", "sset", "DAC", "255", (char *)NULL);
        _exit(127);
    }
}

static void load_volume(void) {
    FILE *f = fopen(VOLUME_FILE, "r");
    if (f) { if (fscanf(f, "%d", &volume) != 1) volume = 7; fclose(f); }
    if (volume < 0) volume = 0;
    if (volume > VOL_STEPS) volume = VOL_STEPS;
    dac_full_once();
    apply_volume();
}

/* ---------- tuning ---------- */

/* The station the needle is on. With 2 detents per station, the in-between
 * click doesn't move it yet: it stays on the station we came from. */
static int committed_steps(void) {
    int r = mod(steps, STEPS_PER_STATION);
    return r == 0 ? steps : steps - (last_dir > 0 ? r : r - STEPS_PER_STATION);
}

static int station_under_dial(void) {
    if (!sched) return -1;
    return mod(committed_steps() / STEPS_PER_STATION, sched->n);
}

/* Where the list should sit: on the station, plus a small nudge toward the
 * next one on the in-between click — a detent you can see, not a glide. */
static double dial_target(void) {
    double t = (double)committed_steps() / STEPS_PER_STATION;
    if (steps != committed_steps()) t += (steps > committed_steps() ? 0.22 : -0.22);
    return t;
}

static int tuning_now(void) {
    if (!sched) return 1;
    return station_under_dial() != tuned || audio_state() == AUDIO_TUNING;
}

static const ep_t *live_ep(int st, int64_t *off_out) {
    int slot; int64_t off;
    if (!sched || st < 0 || sched_live(sched, st, now_ms(), &slot, &off)) return NULL;
    if (off_out) *off_out = off;
    return &sched->st[st].eps[slot];
}

static void tune(int st) {
    if (!sched || st == tuned) return;
    tuned = st;
    audio_tune(st);
    resolver_set_presence(sched->st[st].id);
    save_last_station(sched->st[st].id);
    LOG("tune: %s", sched->st[st].id);
}

static void adopt_schedule(sched_t *s) {
    const char *keep = tuned >= 0 && sched ? sched->st[tuned].id : last_id;
    int idx = 0;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->st[i].id, keep)) idx = i;
    sched_t *old = sched;
    int ref = old ? (tuned >= 0 ? tuned : station_under_dial()) : 0;
    sched = s;
    if (!old) { steps = idx * STEPS_PER_STATION; dial = idx; }
    else { steps += (idx - ref) * STEPS_PER_STATION; dial += idx - ref; }   /* stations may move */
    tuned = -1;
    audio_tune(-1);
    tune(idx);
    sched_free(old);
}

/* Tell the resolver which redirect chains to walk next: the tuned station's
 * current + next episode, then the stations either side of the dial. */
static void update_wants(void) {
    if (!sched) return;
    const char *urls[24];
    int n = 0, order[MAX_STATIONS], no = 0;
    int64_t t = now_ms();
    int center = tuned >= 0 ? tuned : station_under_dial();
    order[no++] = center;
    for (int d = 1; no < sched->n && d <= sched->n; d++) {
        if (no < sched->n) order[no++] = mod(center + d, sched->n);
        if (no < sched->n && mod(center - d, sched->n) != mod(center + d, sched->n)) order[no++] = mod(center - d, sched->n);
    }
    for (int k = 0; k < no && n < 22; k++) {
        int slot; int64_t off;
        if (sched_live(sched, order[k], t, &slot, &off)) continue;
        const station_t *st = &sched->st[order[k]];
        urls[n++] = st->eps[slot].url;
        if (k == 0 || sched_slot_remaining(sched, order[k], t) < 120000)
            urls[n++] = st->eps[(slot + 1) % st->n].url;
    }
    resolver_want(urls, n);

    /* every station's live art, nearest the needle first, so passing stations
     * crossfade without waiting on the network */
    const char *arts[MAX_STATIONS + 2];
    int na = 0, sts[MAX_STATIONS + 1], ns = 0, nd = station_under_dial();
    sts[ns++] = nd;
    for (int d = 1; d <= sched->n / 2 + 1 && ns < sched->n; d++) {
        if (ns < sched->n) sts[ns++] = mod(nd + d, sched->n);
        if (ns < sched->n && mod(nd - d, sched->n) != mod(nd + d, sched->n)) sts[ns++] = mod(nd - d, sched->n);
    }
    for (int k = 0; k < ns; k++) {
        const ep_t *ep = live_ep(sts[k], NULL);
        int dup = 0;
        for (int j = 0; ep && j < na; j++) if (!strcmp(arts[j], ep->image)) dup = 1;
        if (ep && ep->image[0] && !dup) arts[na++] = ep->image;
    }
    if (mode == MODE_FAVE_INFO && fave_sel >= 0 && fave_sel < nfaves && faves[fave_sel].image[0] && na < MAX_STATIONS + 1) {
        memmove(arts + 1, arts, (size_t)na * sizeof arts[0]);   /* most urgent first */
        arts[0] = faves[fave_sel].image;
        na++;
    }
    resolver_want_art(arts, na);
}

/* ---------- input ---------- */

static void fmt_clock(char *out, size_t cap);

/* Bottom-left: the time, then the volume glyph (tap: mute/unmute). Volume
 * itself is the top button held + the knob. */
typedef struct { int icon; } vol_layout_t;
static vol_layout_t vol_layout(void) {
    char clock[16];
    fmt_clock(clock, sizeof clock);
    vol_layout_t v;
    v.icon = 10 + ui_text_width(&font_clock, clock) + 12;   /* left edge of the 18 px glyph */
    return v;
}


/* ---------- system menu (hold the top button) ---------- */

/* Hand control to boot.sh, which acts on /tmp/static-request once we exit. */
static void request_exit(const char *what, const char *msg) {
    FILE *f = fopen("/tmp/static-request", "w");
    if (f) { fprintf(f, "%s\n", what); fclose(f); }
    LOG("exiting for boot.sh: %s", what);
    ui_fill(0, 0, W, H, SITE_INK);
    ui_text_center_fit(&font_show, W / 2, H / 2 + 6, msg, W - 24, SITE_PALE);
    ui_present();
    quit = 1;
}

static int installed_copy_exists(void) { return access(STATIC_DIR "/static-radio", X_OK) == 0; }

#define SYS_ROW_Y 40
#define SYS_ROW_H 52

static void system_tap(int x, int y) {
    if (y < 36 && x >= W - 48) { mode = MODE_PLAYER; return; }   /* close */
    if (y < SYS_ROW_Y) return;
    int row = (y - SYS_ROW_Y) / SYS_ROW_H;
    char ver[32];
    int us = update_state(ver, sizeof ver);
    if (row == 0) request_exit("stock", "Opening chumby settings...");
    else if (row == 1) {
        if (!app_installed()) request_exit("install", "Installing Static...");
        else if (us == UPD_READY) request_exit("update", "Installing the update...");
        else if (us != UPD_CHECKING && us != UPD_DOWNLOADING && us != UPD_OFF) update_check_now();
    } else if (row == 2 && (app_installed() || installed_copy_exists())) {
        int64_t t = now_ms();
        if (sys_confirm && t - sys_confirm_ms < 4000) request_exit("uninstall", "Turning Static off...");
        else { sys_confirm = 1; sys_confirm_ms = t; }
    }
}

static void sys_row(int i, const char *title, const char *sub, uint16_t bg, uint16_t title_ink, uint16_t dim) {
    int y = SYS_ROW_Y + i * SYS_ROW_H;
    ui_fill(12, y + SYS_ROW_H - 2, W - 24, 1, ui_mix(bg, PAPER, 40));
    ui_text_left_fit(&font_show, 14, y + 22, title, W - 28, title_ink);
    ui_text_left_fit(&font_small, 14, y + 41, sub, W - 28, dim);
}

static void draw_system(void) {
    uint16_t bg = SITE_INK, dim = ui_mix(bg, PAPER, 160);
    ui_fill(0, 0, W, H, bg);
    ui_text(&font_clock, 14, 22, "STATIC", SITE_PALE);
    ui_icon(&icon_close, W - 36, 4, PAPER);
    sys_row(0, "Chumby settings", "Wi-Fi and more. Restart to come back.", bg, SITE_PALE, dim);
    char ver[32], sub[96];
    int us = update_state(ver, sizeof ver);
    if (!app_installed())
        sys_row(1, "Install on this chumby", "Runs at every boot, no stick needed.", bg, SITE_TEAL, dim);
    else if (us == UPD_READY) {
        snprintf(sub, sizeof sub, "Version %s. Restarts in a few seconds.", ver);
        sys_row(1, "Install update", sub, bg, SITE_TEAL, dim);
    } else {
        const char *st = us == UPD_CHECKING ? "Checking..." :
                         us == UPD_DOWNLOADING ? "Downloading..." :
                         us == UPD_CURRENT ? "Up to date." :
                         us == UPD_ERROR ? "Couldn't check. Tap to try again." :
                         us == UPD_OFF ? "Updates are off (noupdate file)." : "Checks twice a day.";
        if (us == UPD_DOWNLOADING) snprintf(sub, sizeof sub, "Downloading version %s...", ver);
        else snprintf(sub, sizeof sub, "%s", st);
        sys_row(1, "Check for updates", sub, bg, SITE_PALE, dim);
    }
    if (sys_confirm && now_ms() - sys_confirm_ms >= 4000) sys_confirm = 0;
    if (app_installed() || installed_copy_exists())
        sys_row(2, sys_confirm ? "Tap again to turn off" : "Turn off Static",
                "Back to stock. Your faves are kept.", bg, sys_confirm ? SITE_TEAL : SITE_PALE, dim);
    snprintf(sub, sizeof sub, "Static %s%s", STATIC_VERSION, app_installed() ? "" : " - running from USB");
    ui_text_left_fit(&font_small, 14, H - 10, sub, W - 28, dim);
}

static void on_tap(int x, int y) {
    int64_t t = now_ms();
    if (t - last_tap_ms < TAP_DEBOUNCE_MS) return;   /* resistive panels bounce */
    last_tap_ms = t;
    if (mode == MODE_SYSTEM) { system_tap(x, y); return; }
    if (mode == MODE_QR) { mode = qr_back; return; }             /* any tap: back */
    if ((mode == MODE_INFO || mode == MODE_FAVE_INFO) && qr_url[0] &&
        x >= QR_BTN_X - 6 && y >= QR_BTN_Y - 8 && y < QR_BTN_Y + QR_BTN_H + 8) {
        qr_back = mode; mode = MODE_QR; return;                  /* show the QR big */
    }
    if (mode == MODE_INFO) { mode = MODE_PLAYER; return; }      /* any other tap closes info */
    if (mode == MODE_FAVE_INFO) { mode = MODE_FAVES; return; }   /* back to the list */
    if (mode == MODE_FAVES) {
        if (y < 36 && x >= W - 48) { mode = MODE_PLAYER; return; }   /* close */
        int row = (y - 40) / 48;                                     /* tap a row: its detail */
        int idx = nfaves - 1 - (faves_scroll + row);
        if (y >= 40 && row >= 0 && row < 3 && idx >= 0 && idx < nfaves) {
            fave_sel = idx;
            backfill_fave(&faves[idx]);
            mode = MODE_FAVE_INFO;
        }
        return;
    }

    if (x >= COL_X && sched) {          /* tap a label: scroll it to the needle and tune */
        int rows = (int)lround((double)(y - H / 2) / LABEL_ROW);
        steps = committed_steps() - rows * STEPS_PER_STATION;   /* a label below the needle is the lower index */
        last_knob_ms = t - SETTLE_MS;
        return;
    }
    if (y >= VOL_Y - 8 && x < ART_W) {
        vol_layout_t v = vol_layout();
        if (x >= v.icon - 14 && x < v.icon + 34) { muted = !muted; audio_set_muted(muted); return; }   /* mute */
    }
    if (y < ICON_LIST_Y + 34 && x >= ICON_HEART_X - 8) {
        if (x < ICON_INFO_X - 4) {                              /* heart */
            if (y < ICON_Y + 34) {
                const ep_t *ep = live_ep(station_under_dial(), NULL);
                if (ep) toggle_fave(ep);
                return;
            }
        } else if (y < ICON_LIST_Y - 2) { mode = MODE_INFO; return; }        /* info dot */
        else { mode = MODE_FAVES; faves_scroll = 0; return; }               /* faves list */
    }
    muted = !muted;                                             /* anywhere else on the art */
    audio_set_muted(muted);
}

static void handle_input(int fd) {
    struct input_event ev[32];
    ssize_t r = read(fd, ev, sizeof ev);
    if (r <= 0) return;
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) {
        if (fd == knob_fd && ev[i].type == EV_REL && ev[i].code == REL_WHEEL) {
            if (bend_down) {                                    /* top button held: volume */
                volume += ev[i].value > 0 ? 1 : -1;               /* clockwise = louder */
                if (volume < 0) volume = 0;
                if (volume > VOL_STEPS) volume = VOL_STEPS;
                if (muted && volume > 0) { muted = 0; audio_set_muted(0); }
                bend_turned = 1;
                volume_shown_ms = now_ms();
                apply_volume();
                continue;
            }
            if (mode == MODE_FAVES) {                           /* knob scrolls the list */
                faves_scroll += ev[i].value;
                if (faves_scroll < 0) faves_scroll = 0;
                if (faves_scroll > (nfaves > 3 ? nfaves - 3 : 0)) faves_scroll = nfaves > 3 ? nfaves - 3 : 0;   /* 3 rows visible */
                continue;
            }
            mode = MODE_PLAYER;
            steps -= ev[i].value;   /* reversed: clockwise walks the list the other way */
            if (ev[i].value) last_dir = ev[i].value > 0 ? -1 : 1;
            last_knob_ms = now_ms();
        } else if (fd == touch_fd && ev[i].type == EV_ABS) {
            if (ev[i].code == ABS_X) touch_x = (ev[i].value - TS_X0) * W / TS_XSPAN;
            else if (ev[i].code == ABS_Y) touch_y = (ev[i].value - TS_Y0) * H / TS_YSPAN;
        } else if (fd == touch_fd && ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) {
            if (ev[i].value) touch_down = 1;
            else if (touch_down) { touch_down = 0; if (touch_x >= 0 && touch_y >= 0) on_tap(touch_x, touch_y); }
        } else if (fd == bend_fd && ev[i].type == EV_KEY && ev[i].value == 1) {
            bend_down = 1; bend_turned = 0; hold_opened = 0; bend_down_ms = now_ms();
        } else if (fd == bend_fd && ev[i].type == EV_KEY && ev[i].value == 0 && bend_down) {
            bend_down = 0;
            if (hold_opened) {}                                    /* the hold opened the menu: keep it */
            else if (bend_turned) { volume_shown_ms = now_ms(); LOG("volume %d", volume); }   /* it was a volume turn */
            else if (mode != MODE_PLAYER) mode = MODE_PLAYER;   /* a plain tap closes a panel; mute is the on-screen glyph only */
        }
    }
}

/* ---------- drawing ---------- */

static void fmt_clock(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (clock24) snprintf(out, cap, "%d:%02d", lt.tm_hour, lt.tm_min);
    else snprintf(out, cap, "%d:%02d", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min);
}

/* The phone's listener face: one face per schedule day, the same for everyone
 * (daily_face.dart: dayNumber * 7 mod 11, day = the schedule's UTC date). */
static const icon_t *daily_face(void) {
    int64_t day = sched ? sched->epoch_ms / 86400000 : (int64_t)(time(NULL) - 5 * 3600) / 86400;
    return icon_faces[(int)((day * 7) % 11)];
}

/* Translucent band so text stays legible over the art. */
/* Dark gradient over the art's top/bottom edge so icons read on any cover. */
/* Ink polarity per band, from the art's own brightness (the phone picks icon
 * ink polarity from the cover the same way): light covers get dark ink on a
 * soft light wash, dark covers light ink on a dark wash. */
typedef struct { uint16_t ink, quiet, halo, wash; int wash_max; } polarity_t;
static polarity_t pol_top, pol_bottom;

static polarity_t polarity_for(int y0, int h) {
    long sum = 0, n = 0;
    for (int y = y0; y < y0 + h; y += 3)
        for (int x = 0; x < ART_W; x += 4) {
            uint16_t v = ui_back()[y * W + x];
            sum += ((v >> 11) * 8 * 77 + ((v >> 5) & 63) * 4 * 150 + (v & 31) * 8 * 29) >> 8;
            n++;
        }
    int luma = n ? (int)(sum / n) : 0;
    if (luma > 150) return (polarity_t){ SITE_INK, 0x52AA /* #555 */, 0xFFFF, 0xFFFF, 110 };
    return (polarity_t){ PAPER, QUIET_INK, 0x0000, 0x0000, 150 };
}

static void shade(int y0, int h, int top_dark, const polarity_t *p) {
    for (int r = 0; r < h; r++) {
        int a = top_dark ? p->wash_max * (h - r) / h : p->wash_max * r / h;
        ui_fill_alpha(0, y0 + r, ART_W, 1, p->wash, a);
    }
}

static void icon_on_art(const icon_t *ic, int x, int y, uint16_t c) {
    ui_icon(ic, x + 1, y + 1, pol_top.halo);
    ui_icon(ic, x, y, c);
}

static void text_on_art(const font_t *f, int x, int y, const char *s, uint16_t c) {
    const polarity_t *p = y > H / 2 ? &pol_bottom : &pol_top;
    ui_text(f, x + 1, y + 1, s, p->halo);
    ui_text(f, x, y, s, c);
}

/* Station labels down the right, by the knob. The selected spot stays put in
 * the middle (teal needle) and the list scrolls through it as you turn,
 * wrapping around. */
static void draw_column(void) {
    int n = sched->n;
    ui_fill(COL_X, 0, COL_W, H, SITE_INK);
    int cy0 = H / 2;
    ui_fill(COL_X, cy0 - LABEL_ROW / 2, COL_W, LABEL_ROW, SITE_DARK);
    ui_fill(W - 3, cy0 - LABEL_ROW / 2, 3, LABEL_ROW, SITE_TEAL);   /* the needle, fixed: right edge, by the knob */
    double base = floor(dial), frac = dial - base;
    int reach = H / (2 * LABEL_ROW) + 1;
    for (int k = -reach; k <= reach + 1; k++) {
        int idx = mod((int)base + k, n);
        /* rows from the needle; drawn so clockwise (dial decreasing) moves the
         * list UP and counter-clockwise moves it down */
        double off = frac - k;
        int cy = cy0 + (int)lround(off * LABEL_ROW);
        if (cy < -LABEL_ROW || cy > H + LABEL_ROW) continue;
        double d = fabs(off);
        uint16_t c = d < 0.5 ? SITE_TEAL : ui_mix(SITE_INK, SITE_PALE, d < 1.5 ? 170 : (d < 2.5 ? 120 : 80));
        const char *tag = sched->st[idx].tag;
        const font_t *lf = d < 0.5 ? &font_label_bold : &font_label;   /* the selected one is bold */
        ui_text(lf, COL_X + (COL_W - 3 - ui_text_width(lf, tag)) / 2, cy + 6, tag, c);
    }
}

/* The phone's dotted heart trail (_DottedTrailPainter): straight down out of
 * the heart, an elbow (tightness 0.85), then a short horizontal run into the
 * faves list. Dots every 4 px (phone: 5 pt at 36 px icons; ours are 26 px). */
static void draw_trail(double frac) {
    if (frac <= 0) return;
    double sx = ICON_HEART_X + 13, sy = ICON_Y + 27;           /* under the heart */
    double ex = ICON_LIST_X + 2, ey = ICON_LIST_Y + 13;        /* the list's left edge */
    double cx = ex - 7, k = 0.85;                               /* curve end, then the run */
    double px[80], py[80];
    int np = 0;
    for (int i = 0; i <= 60; i++) {                             /* cubic bezier */
        double t = i / 60.0, u = 1 - t;
        double x1 = sx, y1 = sy + (ey - sy) * k, x2 = sx + (cx - sx) * (1 - k), y2 = ey;
        px[np] = u*u*u*sx + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*cx;
        py[np] = u*u*u*sy + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*ey;
        np++;
    }
    px[np] = ex; py[np] = ey; np++;
    double total = 0;
    for (int i = 1; i < np; i++) total += hypot(px[i] - px[i-1], py[i] - py[i-1]);
    double limit = total * frac, acc = 0, next = 0;
    for (int i = 1; i < np && next <= limit; i++) {
        double seg = hypot(px[i] - px[i-1], py[i] - py[i-1]);
        while (next <= acc + seg && next <= limit) {
            double t = seg > 0 ? (next - acc) / seg : 0;
            int x = (int)lround(px[i-1] + (px[i] - px[i-1]) * t), y = (int)lround(py[i-1] + (py[i] - py[i-1]) * t);
            ui_fill_alpha(x, y, 3, 3, pol_top.halo, 120);       /* halo */
            ui_fill(x - 1, y, 3, 1, pol_top.ink); ui_fill(x, y - 1, 1, 3, pol_top.ink);   /* round-ish dot */
            next += 4;
        }
        acc += seg;
    }
}

/* The art panel (art, washes, top icons, count, trail) is composed only when
 * one of its inputs changes and cached in hero_base; a frame otherwise just
 * copies it back, redraws the bottom bar and the station column, and pushes
 * only those regions to the LCD (a full compose was ~45 ms on this CPU). */
static uint16_t hero_base[ART_W * H];   /* the finished panel for the current station */
static uint16_t hero_from[ART_W * H];   /* what was on screen when a fade began */
static uint16_t hero_disp[ART_W * H];   /* the panel as displayed mid-fade */
static int64_t hero_fade_ms = -100000;
static int hero_fade_was;
#define HERO_FADE_MS 200                /* crossfade between stations' art */
static uint32_t hero_key;
static int hero_valid, present_full = 1;


static void draw_player(void) {
    char buf[32];
    /* the art follows the needle, crossfading even as stations pass by */
    int under = station_under_dial();
    const station_t *st = &sched->st[under];
    uint16_t bg = st->bg;
    const ep_t *ep = live_ep(under, NULL);
    const uint16_t *art = ep ? resolver_art(ep->image) : NULL;
    /* never empty: until this station's art arrives keep the last image up, then
     * crossfade straight from it (a flat color only before any art exists) */
    if (!art && art_have) art = art_shown_raw;
    const void *id = art == art_shown_raw ? art_shown_id : art ? (const void *)art : (const void *)(uintptr_t)(bg + 1);
    int faved = ep && fave_index(fave_key(ep)) >= 0;
    int c = resolver_count(st->id);
    if (under == tuned && c < 1) c = 1;          /* you're listening */
    int64_t since = now_ms() - trail_changed_ms;
    double tf = since >= TRAIL_MS ? 1.0 : (double)since / TRAIL_MS;
    double trail = trail_on ? tf : 1.0 - tf;
    int trail_anim = tf > 0 && tf < 1;
    uint32_t key = (uint32_t)(uintptr_t)id * 2654435761u ^ (uint32_t)bg ^ (uint32_t)faved << 17 ^
                   (uint32_t)c << 20 ^ (uint32_t)trail_on << 30 ^ (uint32_t)(uintptr_t)daily_face();
    int64_t fs = now_ms() - hero_fade_ms;
    int fading = fs < HERO_FADE_MS;
    if (!hero_valid || key != hero_key || trail_anim) {
        /* Compose the finished panel ONCE per change; a crossfade is then one blend
         * between finished panels per frame (~5 ms) instead of a full compose. */
        if (hero_valid && id != art_shown_id) {   /* new art: fade from what's on screen */
            memcpy(hero_from, fading ? hero_disp : hero_base, sizeof hero_from);
            hero_fade_ms = now_ms(); fs = 0; fading = 1;
        }
        art_shown_id = id;
        if (art && art != art_shown_raw) { memcpy(art_shown_raw, art, sizeof art_shown_raw); art_have = 1; }
        /* hero: the phone's riso art, pre-baked server-side (bake_riso_art.yml) */
        if (art) ui_blit_rect(art, 0, 0, ART_W, H);
        else ui_fill(0, 0, ART_W, H, bg);
        pol_top = polarity_for(0, 72);          /* icon rows */
        pol_bottom = polarity_for(H - 40, 40);  /* time + volume */
        shade(0, 46, 1, &pol_top);
        shade(H - 46, 46, 0, &pol_bottom);
        /* icons over the art: face + listeners | heart, faves list, info */
        icon_on_art(daily_face(), 8, ICON_Y, pol_top.ink);
        if (c > 0) {
            if (c < 1000) snprintf(buf, sizeof buf, "%d", c);
            else if (c < 10000) snprintf(buf, sizeof buf, "%.1fk", c / 1000.0);
            else snprintf(buf, sizeof buf, "%dk", (c + 500) / 1000);
            text_on_art(&font_show, 40, ICON_Y + 20, buf, pol_top.ink);
        }
        draw_trail(trail);
        icon_on_art(faved ? &icon_heart_fill : &icon_heart_line, ICON_HEART_X, ICON_Y, pol_top.ink);   /* same ink as the others */
        icon_on_art(&icon_info, ICON_INFO_X, ICON_Y, pol_top.ink);
        ui_icon_rot180(&icon_faves_list, ICON_LIST_X + 1, ICON_LIST_Y + 1, pol_top.halo);
        ui_icon_rot180(&icon_faves_list, ICON_LIST_X, ICON_LIST_Y, pol_top.ink);
        ui_read_rect(hero_base, 0, 0, ART_W, H);   /* the finished panel */
        hero_key = key; hero_valid = 1; present_full = 1;
    } else ui_blit_rect(hero_base, 0, 0, ART_W, H);
    if (fading) {
        ui_blend_rect(hero_from, 0, 0, ART_W, H, 255 - (int)(fs * 255 / HERO_FADE_MS));
        ui_read_rect(hero_disp, 0, 0, ART_W, H);   /* for a fade interrupted by the next station */
        present_full = 1;
    } else if (hero_fade_was) present_full = 1;    /* the frame that lands the fade */
    hero_fade_was = fading;


    /* bottom of the art: time (US Central), volume to its right */
    char clock[16];
    fmt_clock(clock, sizeof clock);
    int base = H - 12;
    vol_layout_t v = vol_layout();
    /* quiet ink, but with the same 1 px halo as the icons so busy covers can't swallow it */
    uint16_t halo = pol_bottom.halo;
    ui_text(&font_clock, 11, base + 1, clock, halo);
    ui_text(&font_clock, 10, base, clock, pol_bottom.quiet);
    uint16_t teal = halo == 0xFFFF ? 0x0450 /* #088A80 on light art */ : SITE_TEAL;
    /* glyphs sized to the clock and centered on its digits */
    const icon_t *vg = muted ? &icon_volume_off_sm : &icon_volume_sm;
    ui_icon(vg, v.icon + 1, base - 14, halo);
    ui_icon(vg, v.icon, base - 15, muted ? teal : pol_bottom.quiet);
    if (bend_down || now_ms() - volume_shown_ms < VOL_OVERLAY_MS) {   /* hold top button + turn */
        int ox = 30, oy = 92, ow = ART_W - 60, oh = 56;
        ui_fill_alpha(ox, oy, ow, oh, SITE_INK, 225);
        ui_fill(ox, oy, ow, 1, ui_mix(SITE_INK, PAPER, 60)); ui_fill(ox, oy + oh - 1, ow, 1, ui_mix(SITE_INK, PAPER, 60));
        ui_text(&font_label, ox + 12, oy + 20, "VOLUME", ui_mix(SITE_INK, SITE_PALE, 170));
        char lv[8];
        snprintf(lv, sizeof lv, "%d", volume);
        ui_text(&font_show, ox + ow - 12 - ui_text_width(&font_show, lv), oy + 22, lv, SITE_PALE);
        int bw = (ow - 24 - 9 * 4) / VOL_STEPS;
        for (int k = 0; k < VOL_STEPS; k++)
            ui_fill(ox + 12 + k * (bw + 4), oy + 32, bw, 12, k < volume ? SITE_TEAL : ui_mix(SITE_INK, PAPER, 50));
        present_full = 1;   /* the overlay sits over the cached art panel */
        vol_overlay_was = 1;
    } else if (vol_overlay_was) { vol_overlay_was = 0; present_full = 1; }   /* repaint where it was */
    if (sched && tuning_now()) {
        /* breathing teal + cycling dots: it's working, not stuck */
        int64_t now = now_ms();
        double ph = (double)(now % 1200) / 1200.0;
        int a = 110 + (int)(145 * (0.5 - 0.5 * cos(2 * M_PI * ph)));
        int offline = offline_since && now - offline_since > OFFLINE_HINT_MS;
        const char *word = offline ? "NO NETWORK" : station_under_dial() != tuned ? "" : "TUNING";
        char t[16];
        int dots = (int)(now / 350 % 4);
        snprintf(t, sizeof t, "%s%.*s", word, offline ? 0 : dots, "...");
        int x = ART_W - 12 - ui_text_width(&font_small, offline ? "NO NETWORK" : "TUNING...");   /* fixed left edge: no jitter */
        text_on_art(&font_small, x, base, t, ui_mix(pol_bottom.halo, teal, a));
    }
    draw_column();
}

static void draw_faves(void) {
    char buf[256];
    const station_t *st = &sched->st[station_under_dial()];
    uint16_t bg = ui_mix(st->bg, 0x0000, 90), dim = ui_mix(bg, PAPER, 160);
    ui_fill(0, 0, W, H, bg);
    char clock[16];
    fmt_clock(clock, sizeof clock);
    ui_text(&font_clock, 10, H - 12, clock, QUIET_INK);
    ui_icon(&icon_heart_fill, 12, 4, PAPER);
    ui_text(&font_clock, 44, 22, "FAVES", PAPER);
    ui_icon(&icon_close, W - 36, 4, PAPER);   /* close (any tap closes too) */
    if (nfaves == 0) {
        ui_text_center_fit(&font_show, W / 2, 120, "No faves yet", 300, PAPER);
        ui_text_center_fit(&font_small, W / 2, 144, "Tap the heart while a show is on.", 300, dim);
    }
    /* newest first; the knob scrolls in half-rows */
    int first = faves_scroll;
    for (int row = 0; row < 3 && first + row < nfaves; row++) {
        const fave_t *f = &faves[nfaves - 1 - (first + row)];
        int y = 40 + row * 48;
        ui_fill(12, y + 46, W - 24, 1, ui_mix(bg, PAPER, 40));
        ui_ascii(f->show[0] ? f->show : f->key, buf, sizeof buf);
        ui_text_left_fit(&font_show, 14, y + 20, buf, W - 28, PAPER);
        ui_ascii(f->title, buf, sizeof buf);
        ui_text_left_fit(&font_small, 14, y + 39, buf, W - 28, dim);
    }
    if (nfaves > 3) {   /* the knob scrolls the list */
        snprintf(buf, sizeof buf, "%d-%d of %d  -  turn the dial to scroll", first + 1, first + 3 > nfaves ? nfaves : first + 3, nfaves);
        ui_text_left_fit(&font_small, 90, H - 12, buf, W - 100, dim);
    }
}

/* QR code for the episode page (cached per URL). Returns its pixel size, or 0
 * if the URL is too long to draw scannably at max_px on this screen. */
static int draw_qr(const char *url, int cx, int cy, int max_px) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
    static char cached[1024];
    static int ok;
    if (strcmp(cached, url)) {
        uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
        snprintf(cached, sizeof cached, "%s", url);
        ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 12, qrcodegen_Mask_AUTO, true);
    }
    if (!ok) return 0;
    int n = qrcodegen_getSize(qr), quiet = 2;
    int scale = max_px / (n + 2 * quiet);
    if (scale < 2) return 0;   /* 1 px modules won't scan off this LCD */
    int size = (n + 2 * quiet) * scale, x0 = cx - size / 2, y0 = cy - size / 2;
    ui_fill(x0, y0, size, size, 0xFFFF);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y)) ui_fill(x0 + (quiet + x) * scale, y0 + (quiet + y) * scale, scale, scale, SITE_INK);
    return size;
}

static const ep_t *find_ep_by_key(const char *key) {
    if (!sched) return NULL;
    for (int i = 0; i < sched->n; i++)
        for (int j = 0; j < sched->st[i].n; j++)
            if (!strcmp(fave_key(&sched->st[i].eps[j]), key)) return &sched->st[i].eps[j];
    return NULL;
}

/* Detail view shared by Info and a fave: the art (clean, no icons over it),
 * the podcast, the episode, an optional line, and a QR code to the episode
 * page — no station name. */
static void draw_detail(const char *show, const char *title, const char *image, const char *link,
                        const char *line, uint16_t base_bg) {
    char buf[512];
    uint16_t bg = ui_mix(base_bg, 0x0000, 90), dim = ui_mix(bg, PAPER, 175);
    ui_fill(0, 0, W, H, bg);
    ui_icon(&icon_close, W - 36, 4, PAPER);
    char clock[16];
    fmt_clock(clock, sizeof clock);
    ui_text(&font_clock, 10, H - 12, clock, QUIET_INK);
    const uint16_t *art = image && image[0] ? resolver_art(image) : NULL;
    if (art) ui_blit_half(art, ART_W, H, 12, 12);              /* 240 -> 120, exact 2:1 */
    else ui_fill(12, 12, 120, 120, base_bg);
    ui_ascii(show, buf, sizeof buf);
    ui_text_wrap(&font_show, 144, 52, W - 144 - 10, 23, 3, buf, PAPER);  /* podcast */
    ui_ascii(title, buf, sizeof buf);
    ui_text_wrap(&font_small, 12, 156, W - 24, 19, 2, buf, PAPER);       /* episode */
    snprintf(qr_url, sizeof qr_url, "%s", link ? link : "");
    int bw = 0;
    if (qr_url[0]) {   /* a quiet button; the QR itself opens big on tap */
        uint16_t edge = ui_mix(bg, PAPER, 90);
        ui_fill(QR_BTN_X, QR_BTN_Y, QR_BTN_W, 1, edge); ui_fill(QR_BTN_X, QR_BTN_Y + QR_BTN_H - 1, QR_BTN_W, 1, edge);
        ui_fill(QR_BTN_X, QR_BTN_Y, 1, QR_BTN_H, edge); ui_fill(QR_BTN_X + QR_BTN_W - 1, QR_BTN_Y, 1, QR_BTN_H, edge);
        ui_icon(&icon_qr, QR_BTN_X + 5, QR_BTN_Y + 3, PAPER);
        ui_text(&font_small, QR_BTN_X + 36, QR_BTN_Y + 21, "Episode page", PAPER);
        bw = QR_BTN_W + 12;
    }
    if (line && line[0]) ui_text_left_fit(&font_small, 12, 199, line, W - 24 - bw, dim);
}

/* The QR, big: white field, centered, with a caption. Any tap goes back. */
static void draw_qr_full(void) {
    ui_fill(0, 0, W, H, 0xFFFF);
    int s = draw_qr(qr_url, W / 2, H / 2, 224);
    if (!s) ui_text_center_fit(&font_show, W / 2, 120, "Link too long to show", 300, SITE_INK);
    ui_icon(&icon_close, W - 36, 4, SITE_INK);   /* any tap goes back, too */
}

static void draw_info(void) {
    int under = station_under_dial();
    const station_t *st = &sched->st[under];
    const ep_t *ep = live_ep(under, NULL);
    if (!ep) { draw_detail(st->tag, "", NULL, NULL, NULL, st->bg); return; }
    draw_detail(ep->show[0] ? ep->show : st->tag, ep->title, ep->image, ep->link, NULL, st->bg);
}

static void draw_fave_info(void) {
    if (fave_sel < 0 || fave_sel >= nfaves) { mode = MODE_FAVES; return; }
    const fave_t *f = &faves[fave_sel];
    draw_detail(f->show[0] ? f->show : f->key, f->title, f->image, f->link, NULL, sched->st[station_under_dial()].bg);
}

static void draw(void) {
    if (quit) return;   /* leave request_exit()'s message up */
    if (mode == MODE_SYSTEM) draw_system();
    else if (!sched) {
        ui_blit_rect(splash_565, 0, 0, W, H);   /* the phone's launch screen */
        if (offline_since && now_ms() - offline_since > OFFLINE_HINT_MS)
            ui_text_center_fit(&font_small, W / 2, H - 14, "No network. Hold the top button for settings.", W - 16, SITE_INK);
    } else if (mode == MODE_FAVES) draw_faves();
    else if (mode == MODE_INFO) draw_info();
    else if (mode == MODE_FAVE_INFO) draw_fave_info();
    else if (mode == MODE_QR) draw_qr_full();
    else draw_player();
    static int last_mode = -1;
    /* Push the whole screen every 2 s anyway: the stock boot scripts draw
     * straight onto the framebuffer ("checking network devices") after we've
     * started, and partial presents would leave that showing under the art. */
    static int64_t last_full;
    int64_t tnow = now_ms();
    if (tnow - last_full > 2000) present_full = 1;
    if (mode != MODE_PLAYER || mode != last_mode || present_full || !sched) { ui_present(); last_full = tnow; }
    else {   /* only what moves: the station column and the bottom bar */
        ui_present_rect(COL_X, 0, COL_W, H);
        ui_present_rect(0, H - 46, ART_W, 46);
    }
    present_full = 0;
    last_mode = mode;
}

/* ---------- main loop ---------- */

static void on_signal(int s) { (void)s; quit = 1; }

/* Watchdog: if the main loop stalls (a hang = frozen screen AND silence, since
 * the mixer runs on this thread), exit so boot.sh restarts us within ~2 s. */
static volatile int64_t loop_beat;
static void *watchdog(void *arg) {
    (void)arg;
    for (;;) {
        sleep(2);
        int64_t beat = loop_beat;
        if (beat && now_ms() - beat > 10000) {
            LOG("watchdog: main loop stalled %lld ms; exiting for a restart", (long long)(now_ms() - beat));
            _exit(3);
        }
        if (quit && beat && now_ms() - beat > 3000) _exit(0);   /* asked to stop but stuck */
    }
    return NULL;
}
/* Debug: `kill -USR1` cycles player -> faves -> info (screenshots without taps). */
static volatile sig_atomic_t cycle_mode;
static void on_usr1(int s) { (void)s; cycle_mode = 1; }
/* Debug: `kill -USR2` opens the big QR from a detail view. */
static volatile sig_atomic_t open_qr;
static void on_usr2(int s) { (void)s; open_qr = 1; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts(STATIC_VERSION); return 0; }   /* the updater's sanity check */
    /* the chumby's own time zone: with TZ unset, musl reads /etc/localtime (-> /psp/localtime) */
    unsetenv("TZ");
    tzset();
    char cf[8] = "";
    FILE *cff = fopen("/psp/clock_format", "r");
    if (cff) { if (!fgets(cf, sizeof cf, cff)) cf[0] = 0; fclose(cff); }
    clock24 = atoi(cf) == 24;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    LOG("static-radio %s starting from %s", STATIC_VERSION, app_dir());
    if (psa_crypto_init() != PSA_SUCCESS) LOG("psa_crypto_init failed");
    if (ui_init()) return 1;
    load_last_station();
    load_volume();
    load_faves();
    trail_on = nfaves > 0;   /* already-saved faves: trail starts fully drawn */
    knob_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    bend_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (knob_fd >= 0) ioctl(knob_fd, EVIOCGRAB, 1);
    FILE *pi = fopen("/sys/module/stmp3xxx_rotdec/parameters/poll_interval", "w");   /* 50 ms -> 10 ms */
    if (pi) { fputs("10", pi); fclose(pi); }
    audio_init();
    resolver_start();
    update_start();
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    draw();

    int64_t last_draw = 0, last_wants = 0;
    while (!quit) {
        loop_beat = now_ms();
        sched_t *fresh = resolver_take_schedule();
        if (fresh) adopt_schedule(fresh);

        struct pollfd fds[12];
        int nf = 0;
        if (knob_fd >= 0) { fds[nf].fd = knob_fd; fds[nf++].events = POLLIN; }
        if (touch_fd >= 0) { fds[nf].fd = touch_fd; fds[nf++].events = POLLIN; }
        if (bend_fd >= 0) { fds[nf].fd = bend_fd; fds[nf++].events = POLLIN; }
        int ninput = nf;
        nf += audio_pollfds(fds + nf, 12 - nf);
        double target = sched ? dial_target() : dial;
        int animating = fabs(target - dial) > 0.002;
        poll(fds, (nfds_t)nf, animating ? 20 : 100);
        int input = 0;
        if (cycle_mode) { cycle_mode = 0; mode = (mode + 1) % 3; input = 1; }
        if (open_qr) { open_qr = 0; if (qr_url[0]) { qr_back = mode; mode = MODE_QR; input = 1; } }
        for (int i = 0; i < ninput; i++) if (fds[i].revents & POLLIN) { handle_input(fds[i].fd); input = 1; }

        audio_set_network_ready(resolver_network_ready());
        if (sched && now_ms() - last_knob_ms > 1500) {   /* pre-buffer the needle's neighbors once it rests */
            int nd = station_under_dial();
            audio_set_neighbors(mod(nd + 1, sched->n), mod(nd - 1, sched->n));
        }
        audio_service(sched);

        int64_t t = now_ms();
        if (sched && tuned != station_under_dial() && t - last_knob_ms > SETTLE_MS) tune(station_under_dial());
        if (sched && t - last_wants > 1000) {
            update_wants(); last_wants = t;
            /* Debug: `echo "tune 3" > /tmp/static-cmd` turns the dial to station 3;
             * "menu" opens the system menu; "tap X Y" taps the screen. */
            FILE *cf = fopen("/tmp/static-cmd", "r");
            if (cf) {
                int n;
                char cmd[16] = "";
                if (fscanf(cf, "%15s", cmd) == 1 && !strcmp(cmd, "vol")) volume_shown_ms = t;   /* reveal -/+ */
                if (!strcmp(cmd, "menu")) { mode = MODE_SYSTEM; sys_confirm = 0; }       /* as if held */
                int tx, ty;
                if (!strcmp(cmd, "tap") && fscanf(cf, "%d %d", &tx, &ty) == 2) { last_tap_ms = 0; on_tap(tx, ty); }
                rewind(cf);
                if (fscanf(cf, "tune %d", &n) == 1 && n >= 0 && n < sched->n) {
                    int cur = station_under_dial();
                    steps = committed_steps() + (n - cur) * STEPS_PER_STATION;
                    last_knob_ms = t;
                    LOG("debug: tune to %d", n);
                }
                fclose(cf);
                unlink("/tmp/static-cmd");
            }
        }

        target = sched ? dial_target() : dial;
        /* quick, crisp settle (~100 ms) rather than a long glide */
        if (fabs(target - dial) > 0.01) dial += (target - dial) * 0.85;   /* snappy: most of the way each frame */
        else dial = target;
        if (bend_down && !bend_turned && !hold_opened && t - bend_down_ms > HOLD_MENU_MS) {
            hold_opened = 1; sys_confirm = 0; mode = MODE_SYSTEM; input = 1;
            if (update_state(NULL, 0) == UPD_ERROR) update_check_now();
        }
        if (resolver_network_ready()) offline_since = 0;
        else if (!offline_since) offline_since = t;
        static int64_t last_upd_look;
        if (t - last_upd_look > 60000) {   /* a staged update installs in the 4 a.m. hour */
            last_upd_look = t;
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            if (lt.tm_hour == 4 && update_state(NULL, 0) == UPD_READY && mode == MODE_PLAYER)
                request_exit("update", "Installing the update...");
        }
        if (mode == MODE_SYSTEM) animating = 1;   /* live update status */
        if (t - trail_changed_ms < TRAIL_MS + 50 || t - hero_fade_ms < HERO_FADE_MS + 50) animating = 1;
        if (sched && mode == MODE_PLAYER && tuning_now()) animating = 1;   /* breathing "TUNING..." */
        if (bend_down || t - volume_shown_ms < VOL_OVERLAY_MS + 100) animating = 1;   /* volume overlay */
        if (input || t - last_draw > (animating ? 25 : 500)) {
            struct timeval a, b;
            gettimeofday(&a, NULL);
            draw(); last_draw = t;
            gettimeofday(&b, NULL);
            static int draws; static int64_t since, us_sum, us_max;
            int64_t us = (int64_t)(b.tv_sec - a.tv_sec) * 1000000 + (b.tv_usec - a.tv_usec);
            draws++; us_sum += us; if (us > us_max) us_max = us;
            if (t - since > 30000) {
                LOG("ui: %d draws in %llds, avg %lld us, max %lld us", draws, (long long)((t - since) / 1000),
                    (long long)(draws ? us_sum / draws : 0), (long long)us_max);
                draws = 0; since = t; us_sum = us_max = 0;
            }
        }
    }
    LOG("static-radio exiting");
    audio_shutdown();
    return 0;
}
