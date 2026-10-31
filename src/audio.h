#pragma once
#include <poll.h>
#include "sched.h"

enum { AUDIO_IDLE, AUDIO_TUNING, AUDIO_PLAYING, AUDIO_GAP };

void audio_init(void);
void audio_tune(int station);          /* -1 = off */
void audio_set_muted(int muted);
/* Master volume step (0..steps), applied as a gain in the mixer. */
void audio_set_volume(int v, int steps);
/* Until the first live schedule arrives (= network up, fresh timeline) no
 * decoder is launched: at boot ffmpeg would sit in DNS/TCP retries. */
void audio_set_network_ready(int ready);
/* Stations to keep pre-buffered (either side of the needle); -1 = none. */
void audio_set_neighbors(int a, int b);
void audio_schedule_changed(void);     /* kill streams whose slot/url no longer match */

/* Fill pollfds the engine needs; returns count. */
int audio_pollfds(struct pollfd *fds, int max);
/* Read decoder pipes, write output, launch/stop decoders. */
void audio_service(const sched_t *s);
int audio_state(void);
void audio_shutdown(void);
