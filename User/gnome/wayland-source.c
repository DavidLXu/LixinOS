#include "wayland-source.h"
#include <lixin-wayland-loop.h>
#include <wayland-server-core.h>

struct watch {
    int fd;
    GIOCondition events;
    gpointer tag;
};

struct wayland_source {
    GSource base;
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct watch watches[LX_WAYLAND_MAX_POLL_FDS];
    unsigned count;
    gint64 deadline;
};

static gboolean prepare(GSource *base, gint *timeout)
{
    struct wayland_source *source = (struct wayland_source *)base;
    struct pollfd fds[LX_WAYLAND_MAX_POLL_FDS];
    *timeout = -1;
    wl_display_flush_clients(source->display);
    int count = lx_wayland_event_loop_pollfds(source->loop, fds, G_N_ELEMENTS(fds), timeout);
    if (count < 0)
        return TRUE;
    source->deadline = *timeout < 0 ? -1 : g_get_monotonic_time() + *timeout * (gint64)1000;

    unsigned used = 0;
    for (int i = 0; i < count; ++i) {
        if (fds[i].fd < 0)
            continue;
        GIOCondition events = G_IO_ERR | G_IO_HUP | G_IO_NVAL;
        if (fds[i].events & POLLIN) events |= G_IO_IN;
        if (fds[i].events & POLLOUT) events |= G_IO_OUT;
        struct watch *watch = &source->watches[used++];
        if (watch->tag && watch->fd != fds[i].fd) {
            g_source_remove_unix_fd(base, watch->tag);
            watch->tag = NULL;
        }
        if (!watch->tag)
            watch->tag = g_source_add_unix_fd(base, fds[i].fd, events);
        else if (watch->events != events)
            g_source_modify_unix_fd(base, watch->tag, events);
        watch->fd = fds[i].fd;
        watch->events = events;
    }
    for (unsigned i = used; i < source->count; ++i) {
        g_source_remove_unix_fd(base, source->watches[i].tag);
        source->watches[i].tag = NULL;
    }
    source->count = used;
    return *timeout == 0;
}

static gboolean check(GSource *base)
{
    struct wayland_source *source = (struct wayland_source *)base;
    if (source->deadline >= 0 && g_get_monotonic_time() >= source->deadline)
        return TRUE;
    for (unsigned i = 0; i < source->count; ++i)
        if (g_source_query_unix_fd(base, source->watches[i].tag))
            return TRUE;
    return FALSE;
}

static gboolean dispatch(GSource *base, GSourceFunc callback, gpointer data)
{
    (void)callback;
    (void)data;
    struct wayland_source *source = (struct wayland_source *)base;
    if (wl_event_loop_dispatch(source->loop, 0) < 0)
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

GSource *lx_wayland_source_new(struct wl_display *display)
{
    static GSourceFuncs callbacks = {.prepare = prepare, .check = check, .dispatch = dispatch};
    GSource *base = g_source_new(&callbacks, sizeof(struct wayland_source));
    struct wayland_source *source = (struct wayland_source *)base;
    source->display = display;
    source->loop = wl_display_get_event_loop(display);
    source->deadline = -1;
    g_source_set_name(base, "[LixinOS] Wayland events");
    return base;
}
