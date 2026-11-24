#pragma once

#include <poll.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Private to libwayland's event-loop.c. These handles are never exposed
 * as POSIX file descriptors or used by other applications. */
int lx_wayland_poll_open(void);
int lx_wayland_poll_ctl(int loop, int op, int fd, struct epoll_event *event);
int lx_wayland_poll_wait(int loop, struct epoll_event *events, int size, int timeout);
int lx_wayland_poll_prepare(int loop, struct pollfd *fds, unsigned capacity, int *timeout);
int lx_wayland_timer_open(int clock, int flags);
int lx_wayland_timer_set(int fd, int flags, const struct itimerspec *value, struct itimerspec *old);
int lx_wayland_poll_close(int fd);

#define wl_os_epoll_create_cloexec lx_wayland_poll_open
#define epoll_ctl lx_wayland_poll_ctl
#define epoll_wait lx_wayland_poll_wait
#define timerfd_create lx_wayland_timer_open
#define timerfd_settime lx_wayland_timer_set
#define close lx_wayland_poll_close
