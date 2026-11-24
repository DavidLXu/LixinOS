/* Port the subset of epoll and timerfd used by the upstream Wayland loop
 * to LixinOS's poll and monotonic clock. */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "lixin-wayland-loop.h"

enum { LOOP_BASE = 65536, LOOP_COUNT = 8, TIMER_BASE = LOOP_BASE + LOOP_COUNT,
       TIMER_COUNT = 8, WATCHES = LX_WAYLAND_MAX_POLL_FDS };

struct watch {
    int fd;
    struct epoll_event event;
};
struct poll_loop {
    bool used;
    unsigned count;
    struct watch watches[WATCHES];
};
struct timer {
    bool used, armed;
    int64_t deadline;
};

static struct poll_loop loops[LOOP_COUNT];
static struct timer timers[TIMER_COUNT];

static int fail(int error)
{
    errno = error;
    return -1;
}

static struct poll_loop *find_loop(int fd)
{
    return fd >= LOOP_BASE && fd < TIMER_BASE && loops[fd - LOOP_BASE].used
        ? &loops[fd - LOOP_BASE] : NULL;
}

static struct timer *find_timer(int fd)
{
    return fd >= TIMER_BASE && fd < TIMER_BASE + TIMER_COUNT && timers[fd - TIMER_BASE].used
        ? &timers[fd - TIMER_BASE] : NULL;
}

int lx_wayland_poll_open(void)
{
    for (unsigned i = 0; i < LOOP_COUNT; ++i)
        if (!loops[i].used) {
            loops[i] = (struct poll_loop){.used = true};
            return LOOP_BASE + i;
        }
    return fail(EMFILE);
}

int lx_wayland_poll_close(int fd)
{
    struct poll_loop *loop = find_loop(fd);
    if (loop) {
        loop->used = false;
        return 0;
    }
    struct timer *timer = find_timer(fd);
    if (timer) {
        timer->used = false;
        return 0;
    }
    if (fd >= LOOP_BASE) return fail(EBADF);
    return close(fd);
}

int lx_wayland_poll_ctl(int handle, int op, int fd, struct epoll_event *event)
{
    struct poll_loop *loop = find_loop(handle);
    if (!loop || fd < 0 || (fd >= LOOP_BASE && !find_timer(fd))) return fail(EBADF);
    unsigned i = 0;
    while (i < loop->count && loop->watches[i].fd != fd) ++i;
    switch (op) {
    case EPOLL_CTL_DEL:
        if (i == loop->count) return fail(ENOENT);
        loop->watches[i] = loop->watches[--loop->count];
        return 0;
    case EPOLL_CTL_ADD:
        if (i < loop->count) return fail(EEXIST);
        if (loop->count == WATCHES) return fail(ENOSPC);
        break;
    case EPOLL_CTL_MOD:
        if (i == loop->count) return fail(ENOENT);
        break;
    default:
        return fail(EINVAL);
    }
    if (!event || event->events & ~(EPOLLIN | EPOLLOUT)) return fail(EINVAL);
    loop->watches[i] = (struct watch){fd, *event};
    if (op == EPOLL_CTL_ADD) ++loop->count;
    return 0;
}

static int64_t nanoseconds(struct timespec time)
{
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int64_t monotonic(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return nanoseconds(now);
}

int lx_wayland_timer_open(int clock, int flags)
{
    if (clock != CLOCK_MONOTONIC || flags & ~(TFD_CLOEXEC | TFD_NONBLOCK))
        return fail(EINVAL);
    for (unsigned i = 0; i < TIMER_COUNT; ++i)
        if (!timers[i].used) {
            timers[i] = (struct timer){.used = true};
            return TIMER_BASE + i;
        }
    return fail(EMFILE);
}

int lx_wayland_timer_set(int fd, int flags, const struct itimerspec *value, struct itimerspec *old)
{
    struct timer *timer = find_timer(fd);
    if (!timer) return fail(EBADF);
    if (!value || old || flags & ~TFD_TIMER_ABSTIME ||
        value->it_interval.tv_sec || value->it_interval.tv_nsec ||
        value->it_value.tv_sec < 0 || value->it_value.tv_sec > (INT64_MAX / 1000000000 - 2) ||
        value->it_value.tv_nsec < 0 || value->it_value.tv_nsec >= 1000000000)
        return fail(EINVAL);
    int64_t deadline = nanoseconds(value->it_value);
    timer->armed = deadline != 0;
    if (timer->armed && !(flags & TFD_TIMER_ABSTIME)) {
        int64_t now = monotonic();
        deadline = deadline > INT64_MAX - now ? INT64_MAX : deadline + now;
    }
    timer->deadline = deadline;
    return 0;
}

int lx_wayland_poll_prepare(int handle, struct pollfd *fds, unsigned capacity, int *timeout)
{
    struct poll_loop *loop = find_loop(handle);
    if (!loop) return fail(EBADF);
    if (!timeout || *timeout < -1 || (!fds && loop->count)) return fail(EINVAL);
    if (capacity < loop->count) return fail(ENOSPC);
    int64_t now = monotonic();
    for (unsigned i = 0; i < loop->count; ++i) {
        struct watch *watch = &loop->watches[i];
        struct timer *timer = find_timer(watch->fd);
        fds[i] = (struct pollfd){.fd = timer ? -1 : watch->fd};
        if (watch->event.events & EPOLLIN) fds[i].events |= POLLIN;
        if (watch->event.events & EPOLLOUT) fds[i].events |= POLLOUT;
        if (timer && timer->armed) {
            int64_t remaining = timer->deadline - now;
            int64_t ms = remaining <= 0 ? 0 : (remaining - 1) / 1000000 + 1;
            if (ms <= INT_MAX && (*timeout < 0 || ms < *timeout)) *timeout = (int)ms;
        }
    }
    return (int)loop->count;
}

int lx_wayland_poll_wait(int handle, struct epoll_event *events, int size, int timeout)
{
    if (!events || size <= 0) return fail(EINVAL);
    struct pollfd fds[WATCHES];
    int watches = lx_wayland_poll_prepare(handle, fds, WATCHES, &timeout);
    if (watches < 0) return -1;
    struct poll_loop *loop = find_loop(handle);
    int polled = poll(fds, watches, timeout);
    if (polled < 0) return -1;
    int64_t now = monotonic();
    int count = 0;
    for (int i = 0; i < watches && count < size; ++i) {
        struct timer *timer = find_timer(loop->watches[i].fd);
        short revents = fds[i].revents;
        uint32_t ready = 0;
        if ((revents & POLLIN) || (timer && timer->armed && now >= timer->deadline))
            ready |= EPOLLIN;
        if (revents & POLLOUT) ready |= EPOLLOUT;
        if (revents & POLLHUP) ready |= EPOLLHUP;
        if (revents & (POLLERR | POLLNVAL)) ready |= EPOLLERR;
        if (ready) {
            events[count] = loop->watches[i].event;
            events[count++].events = ready;
        }
    }
    return count;
}
