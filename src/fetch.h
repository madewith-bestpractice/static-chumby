#pragma once
#include <stddef.h>
#include <stdint.h>

/* Stream `url` from byte `from` into `fd` (ffmpeg's stdin) on a background
 * thread, over our own TLS (one request, no redirects: pass a resolved URL).
 * Reconnects from the current byte on a dropped connection. The thread owns
 * and closes `fd`; it ends when the file ends or the reader goes away. */
int fetch_start(const char *url, int64_t from, int fd);

/* Neighbor pre-buffer: stream `url` from `start` into a rolling in-memory
 * window (`cap` bytes), reading no further than the ahead limit, until a
 * player attaches. On attach the same connection serves the player: bytes
 * from `from` (which must be inside the window) go to `fd`, then live. */
typedef struct prebuf prebuf_t;
prebuf_t *prebuf_start(const char *url, int64_t start, int64_t ahead_limit, size_t cap);
void prebuf_set_limit(prebuf_t *p, int64_t ahead_limit);
/* 0 on success: the pre-buffer now owns fd and becomes that player's feed.
 * The caller must not touch p again afterwards. */
int prebuf_attach(prebuf_t *p, int64_t from, int fd);
/* Window currently held (for diagnostics / choosing). */
void prebuf_window(prebuf_t *p, int64_t *lo, int64_t *hi);
/* Stop and free (only if not attached). */
void prebuf_cancel(prebuf_t *p);
