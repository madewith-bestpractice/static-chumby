#pragma once
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* Data (faves, caches, settings) always lives on the chumby's own storage; the
 * program may run from there (installed) or from a USB stick (app_dir()). */
#define STATIC_DIR "/mnt/storage/static"
#ifndef STATIC_VERSION
#define STATIC_VERSION "0.0.0-dev"
#endif
/* The latest release's manifest (see Makefile `dist`): {"version","url","sha256","size"} */
#define UPDATE_MANIFEST_URL "https://github.com/madewith-bestpractice/static-chumby/releases/latest/download/latest.json"

const char *app_dir(void);          /* directory holding the running binary */
const char *app_file(const char *name);   /* app_dir()/name (static storage, interned per name) */
int app_installed(void);            /* running from STATIC_DIR (not a USB stick) */
#define USER_AGENT "StaticRadio/chumby (+https://staticradio.app)"
#define SCHEDULE_URL "https://data.staticradio.app/schedule-wear.json"

/* Phone parity (sync_controller.dart _kInterSegmentGapMillis). */
#define GAP_MS 1500
/* Phone parity (radio_audio_engine.dart): bulletins never play past 30 s. */
#define BULLETIN_CAP_MS 30000

static inline int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

#define LOG(...) do { \
    struct timeval _tv; gettimeofday(&_tv, NULL); \
    fprintf(stderr, "[%ld.%03ld] ", (long)_tv.tv_sec, (long)(_tv.tv_usec / 1000)); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); \
} while (0)
