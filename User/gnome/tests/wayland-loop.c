#define _GNU_SOURCE
#include "wayland-source.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct fixture {
    GMainLoop *main;
    int write_fd;
    gboolean saw_pipe, saw_timer;
};

static void send_byte(void *data)
{
    struct fixture *fixture = data;
    if (write(fixture->write_fd, "x", 1) != 1)
        g_main_loop_quit(fixture->main);
}

static int read_byte(int fd, uint32_t mask, void *data)
{
    struct fixture *fixture = data;
    char byte;
    if ((mask & WL_EVENT_READABLE) && read(fd, &byte, 1) == 1 && byte == 'x')
        fixture->saw_pipe = TRUE;
    if (fixture->saw_pipe && fixture->saw_timer)
        g_main_loop_quit(fixture->main);
    return 0;
}

static int timer(void *data)
{
    struct fixture *fixture = data;
    fixture->saw_timer = TRUE;
    if (fixture->saw_pipe)
        g_main_loop_quit(fixture->main);
    return 0;
}

static gboolean timeout(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    struct wl_display *display = wl_display_create();
    if (!display) return 1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        wl_display_destroy(display);
        return 1;
    }
    struct fixture fixture = {g_main_loop_new(NULL, FALSE), fds[1], FALSE, FALSE};
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    struct wl_event_source *reader = wl_event_loop_add_fd(loop, fds[0], WL_EVENT_READABLE,
                                                          read_byte, &fixture);
    struct wl_event_source *tick = wl_event_loop_add_timer(loop, timer, &fixture);
    if (reader && tick && wl_event_source_timer_update(tick, 20) == 0 &&
        wl_event_loop_add_idle(loop, send_byte, &fixture)) {
        GSource *source = lx_wayland_source_new(display);
        GSource *stop = g_timeout_source_new(2000);
        g_source_set_callback(stop, timeout, fixture.main, NULL);
        g_source_attach(source, NULL);
        g_source_attach(stop, NULL);
        g_main_loop_run(fixture.main);
        g_source_destroy(stop);
        g_source_unref(stop);
        g_source_destroy(source);
        g_source_unref(source);
    }

    if (reader) wl_event_source_remove(reader);
    if (tick) wl_event_source_remove(tick);
    close(fds[0]);
    close(fds[1]);
    g_main_loop_unref(fixture.main);
    wl_display_destroy(display);

    if (!fixture.saw_pipe || !fixture.saw_timer) {
        fprintf(stderr, "Wayland event source missed pipe=%d or timer=%d\n",
                fixture.saw_pipe, fixture.saw_timer);
        return 1;
    }
    puts("GLib dispatched Wayland idle, pipe, and timer events");
    return 0;
}
