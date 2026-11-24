#ifndef LIXINOS_WAYLAND_LOOP_H
#define LIXINOS_WAYLAND_LOOP_H

#include <poll.h>

struct wl_event_loop;

#ifdef __cplusplus
extern "C" {
#endif

enum { LX_WAYLAND_MAX_POLL_FDS = 35 };

/* Get the watches for this dispatch and shorten *timeout_ms for pending
   idles or timers. Negative descriptor slots represent internal timers.
   Refresh before each outer poll and call wl_event_loop_dispatch(loop, 0)
   when a watch is ready or the timeout expires. */
int lx_wayland_event_loop_pollfds(struct wl_event_loop *loop, struct pollfd *fds,
                                 unsigned capacity, int *timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
