#pragma once

/* Direct ALSA output thread reading s16 stereo 44.1 kHz PCM from read_fd.
 * Returns 0 if it started (it then owns read_fd), -1 to fall back to aplay. */
int pcmout_start(int read_fd);
int pcmout_running(void);   /* the thread is alive */
int pcmout_done(void);      /* it has exited (EOF or a device error) */
int pcmout_queued_frames(void);   /* audio past the pipe not yet heard (device delay + held) */
