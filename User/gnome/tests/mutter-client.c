#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client.h"

enum { WIDTH = 128, HEIGHT = 96, BYTES = WIDTH * HEIGHT * 4 };

struct client {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    int configured, painted;
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    struct client *client = data;
    (void)version;
    if (!strcmp(interface, wl_compositor_interface.name))
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, wl_shm_interface.name))
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        client->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}

static void removed(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static void ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(shell, serial);
}

static void configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct client *client = data;
    xdg_surface_ack_configure(surface, serial);
    client->configured = 1;
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct client *client = data;
    (void)time;
    client->painted = 1;
    wl_callback_destroy(callback);
}

static const struct wl_registry_listener registry_listener = {global, removed};
static const struct xdg_wm_base_listener shell_listener = {ping};
static const struct xdg_surface_listener surface_listener = {configure};
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};
static const struct wl_callback_listener callback_listener = {frame_done};

static struct wl_buffer *make_buffer(struct wl_shm *shm, int updated) {
    int fd = memfd_create("mutter-check", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, BYTES) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x)
            pixels[y * WIDTH + x] = (x < WIDTH / 2 ? 0x38b694 : 0xec713d);
    if (updated) {
        for (unsigned y = 11; y < 20; ++y)
            for (unsigned x = 7; x < 20; ++x)
                pixels[y * WIDTH + x] = 0xb935ce;
        for (unsigned y = HEIGHT - 5; y < HEIGHT; ++y)
            for (unsigned x = WIDTH - 3; x < WIDTH; ++x)
                pixels[y * WIDTH + x] = 0x42a9ed;
    }
    munmap(pixels, BYTES);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, BYTES);
    close(fd);
    if (!pool)
        return NULL;
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT,
                                                          WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return buffer;
}

int main(int argc, char **argv) {
    const char *socket = argc >= 2 ? argv[1] : NULL;
    struct wl_display *display = NULL;
    for (unsigned attempt = 0; attempt < 300 && !display; ++attempt) {
        display = wl_display_connect(socket);
        if (!display)
            usleep(20000);
    }
    if (!display) {
        perror("Mutter Wayland connection");
        return 1;
    }

    struct client client = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &client);
    if (wl_display_roundtrip(display) < 0 || !client.compositor || !client.shm || !client.shell)
        return 1;

    xdg_wm_base_add_listener(client.shell, &shell_listener, NULL);
    struct wl_surface *surface = wl_compositor_create_surface(client.compositor);
    struct xdg_surface *shell_surface = xdg_wm_base_get_xdg_surface(client.shell, surface);
    xdg_surface_add_listener(shell_surface, &surface_listener, &client);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(shell_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "LixinOS Wayland check");
    wl_surface_commit(surface);
    while (!client.configured)
        if (wl_display_dispatch(display) < 0)
            return 1;

    struct wl_buffer *buffer = make_buffer(client.shm, 0);
    if (!buffer)
        return 1;
    struct wl_callback *callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &callback_listener, &client);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    while (!client.painted)
        if (wl_display_dispatch(display) < 0)
            return 1;

    puts("Mutter painted the xdg-shell client frame");
    fflush(stdout);
    if (argc >= 3 && !strcmp(argv[2], "hold")) {
        struct wl_buffer *updated = make_buffer(client.shm, 1);
        if (!updated)
            return 1;
        client.painted = 0;
        callback = wl_surface_frame(surface);
        wl_callback_add_listener(callback, &callback_listener, &client);
        wl_surface_attach(surface, updated, 0, 0);
        wl_surface_damage(surface, 7, 11, 13, 9);
        wl_surface_damage(surface, WIDTH - 3, HEIGHT - 5, 3, 5);
        wl_surface_commit(surface);
        while (!client.painted)
            if (wl_display_dispatch(display) < 0)
                return 1;
        sleep(3);
        wl_buffer_destroy(updated);
    }
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(shell_surface);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    wl_display_disconnect(display);
    return 0;
}
