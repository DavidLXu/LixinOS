#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "../console.h"
#include "../gui/lixin-gui.h"
#include "demo-pattern.h"

struct demo {
    struct lx_canvas canvas;
    struct wl_event_source *timer;
    int graphical, received_frame, timer_fired, error;
};

struct surface {
    struct demo *demo;
    struct wl_resource *buffer;
    struct wl_listener buffer_destroy;
    struct wl_list callbacks;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int timer_fired(void *data)
{
    ((struct demo *)data)->timer_fired = 1;
    return 0;
}

static int render(struct demo *demo, struct wl_shm_buffer *buffer)
{
    if (!buffer || wl_shm_buffer_get_width(buffer) != DEMO_WIDTH ||
        wl_shm_buffer_get_height(buffer) != DEMO_HEIGHT ||
        wl_shm_buffer_get_stride(buffer) != DEMO_WIDTH * 4 ||
        wl_shm_buffer_get_format(buffer) != WL_SHM_FORMAT_XRGB8888)
        return -1;

    wl_shm_buffer_begin_access(buffer);
    const uint32_t *pixels = wl_shm_buffer_get_data(buffer);
    int valid = 1;
    for (unsigned y = 0; valid && y < DEMO_HEIGHT; ++y)
        for (unsigned x = 0; x < DEMO_WIDTH; ++x)
            if (pixels[y * DEMO_WIDTH + x] != demo_pixel(x, y)) {
                valid = 0;
                break;
            }

    if (valid && demo->graphical) {
        struct lx_canvas *canvas = &demo->canvas;
        unsigned width = canvas->width < DEMO_WIDTH ? canvas->width : DEMO_WIDTH;
        unsigned height = canvas->height < DEMO_HEIGHT ? canvas->height : DEMO_HEIGHT;
        unsigned left = (canvas->width - width) / 2, top = (canvas->height - height) / 2;
        lx_gui_rect(canvas, 0, 0, canvas->width, canvas->height, 0x121a28);
        for (unsigned y = 0; y < height; ++y)
            memcpy(canvas->pixels + (top + y) * canvas->width + left, pixels + y * DEMO_WIDTH, width * 4);
        valid = lx_gui_present(canvas) == 0;
    }
    wl_shm_buffer_end_access(buffer);
    return valid ? 0 : -1;
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void clear_buffer(struct surface *surface)
{
    if (surface->buffer)
        wl_list_remove(&surface->buffer_destroy.link);
    surface->buffer = NULL;
}

static void buffer_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    struct surface *surface = wl_container_of(listener, surface, buffer_destroy);
    clear_buffer(surface);
}

static void surface_destroyed(struct wl_resource *resource)
{
    struct surface *surface = wl_resource_get_user_data(resource);
    clear_buffer(surface);
    while (!wl_list_empty(&surface->callbacks))
        wl_resource_destroy(wl_resource_from_link(surface->callbacks.next));
    free(surface);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    (void)client;
    (void)x;
    (void)y;
    struct surface *surface = wl_resource_get_user_data(resource);
    clear_buffer(surface);
    surface->buffer = buffer;
    if (buffer)
        wl_resource_add_destroy_listener(buffer, &surface->buffer_destroy);
}

static void frame_destroyed(struct wl_resource *resource)
{
    wl_list_remove(wl_resource_get_link(resource));
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct surface *surface = wl_resource_get_user_data(resource);
    struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!callback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(callback, NULL, NULL, frame_destroyed);
    wl_list_insert(surface->callbacks.prev, wl_resource_get_link(callback));
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct surface *surface = wl_resource_get_user_data(resource);
    struct demo *demo = surface->demo;
    struct wl_resource *buffer = surface->buffer;
    clear_buffer(surface);
    if (buffer) {
        if (render(demo, wl_shm_buffer_get(buffer)) < 0 ||
            (!demo->received_frame && wl_event_source_timer_update(demo->timer, 10) < 0))
            demo->error = 1;
        else
            demo->received_frame = 1;
        wl_buffer_send_release(buffer);
    }
    while (!wl_list_empty(&surface->callbacks)) {
        struct wl_resource *callback = wl_resource_from_link(surface->callbacks.next);
        wl_callback_send_done(callback, (uint32_t)now_ms());
        wl_resource_destroy(callback);
    }
}

static void ignore_rect(struct wl_client *client, struct wl_resource *resource,
                        int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void ignore_region(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *region)
{
    (void)client;
    (void)resource;
    (void)region;
}

static const struct wl_surface_interface surface_requests = {
    .destroy = destroy_resource,
    .attach = surface_attach,
    .damage = ignore_rect,
    .frame = surface_frame,
    .set_opaque_region = ignore_region,
    .set_input_region = ignore_region,
    .commit = surface_commit,
};

static const struct wl_region_interface region_requests = {
    .destroy = destroy_resource,
    .add = ignore_rect,
    .subtract = ignore_rect,
};

static void create_surface(struct wl_client *client, struct wl_resource *parent, uint32_t id)
{
    struct surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wl_surface_interface, 1, id);
    if (!resource) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    surface->demo = wl_resource_get_user_data(parent);
    surface->buffer_destroy.notify = buffer_destroyed;
    wl_list_init(&surface->callbacks);
    wl_resource_set_implementation(resource, &surface_requests, surface, surface_destroyed);
}

static void create_region(struct wl_client *client, struct wl_resource *parent, uint32_t id)
{
    (void)parent;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    if (region)
        wl_resource_set_implementation(region, &region_requests, NULL, NULL);
    else
        wl_client_post_no_memory(client);
}

static const struct wl_compositor_interface compositor_requests = {
    .create_surface = create_surface,
    .create_region = create_region,
};

static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    if (resource)
        wl_resource_set_implementation(resource, &compositor_requests, data, NULL);
    else
        wl_client_post_no_memory(client);
}

static long start_client(const char *socket_path)
{
    static const char args[] = ".\0/bin/wayland-shm-client\0wayland-shm-client";
    char *mail = (char *)0x443d8000;
    size_t length = sizeof(args) + strlen(socket_path) + 1;
    memcpy(mail, args, sizeof(args));
    strcpy(mail + sizeof(args), socket_path);
    /* Close stdin in the child so the server keeps the graphical terminal's input. */
    return console_call(50, ((unsigned long)length << 32) | 0x0201ff);
}

int main(int argc, char **argv)
{
    struct demo demo = {.graphical = argc == 1};
    if (!demo.graphical && (argc != 2 || strcmp(argv[1], "--headless"))) {
        fprintf(stderr, "usage: wayland-demo [--headless]\n");
        return 2;
    }
    if (demo.graphical && lx_gui_open(&demo.canvas) < 0) {
        fprintf(stderr, "Wayland demo needs a graphical terminal or --headless\n");
        return 1;
    }

    struct wl_display *display = wl_display_create();
    if (!display) {
        perror("Wayland display");
        return 1;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    char address[64];
    snprintf(address, sizeof(address), "/tmp/lixin-wayland-%ld", (long)getpid());
    int status = 1;
    demo.timer = wl_event_loop_add_timer(loop, timer_fired, &demo);
    if (!demo.timer || wl_display_init_shm(display) < 0 ||
        !wl_global_create(display, &wl_compositor_interface, 1, &demo, bind_compositor) ||
        wl_display_add_socket(display, address) < 0) {
        perror("Wayland server setup");
        goto done;
    }

    long child = start_client(address);
    if (child < 0) {
        fprintf(stderr, "Could not start the Wayland demo client\n");
        goto done;
    }

    long child_status = -2;
    int64_t deadline = now_ms() + 5000;
    while (!demo.error) {
        if (wl_event_loop_dispatch(loop, 20) < 0) {
            perror("Wayland event loop");
            demo.error = 1;
            break;
        }
        wl_display_flush_clients(display);
        if (child_status == -2)
            child_status = console_call(51, (1UL << 32) | (unsigned)child);
        if (child_status != -2 && child_status != 0)
            break;
        if (demo.received_frame && demo.timer_fired && child_status == 0) {
            if (!demo.graphical)
                break;
            int key = lx_timed_byte(1);
            if (key == 27 || key == 'q' || key == 'Q')
                break;
        } else if (now_ms() >= deadline) {
            break;
        }
    }
    if (child_status == -2) {
        wl_display_destroy_clients(display);
        console_call(55, (unsigned)child);
        child_status = console_call(51, (unsigned)child);
    }
    if (demo.received_frame && demo.timer_fired && !demo.error && child_status == 0) {
        puts("Wayland shared buffer and timer passed");
        status = 0;
    } else {
        fprintf(stderr, "Wayland demo failed (frame=%d, timer=%d, error=%d, child=%ld)\n",
                demo.received_frame, demo.timer_fired, demo.error, child_status);
    }

done:
    if (demo.timer) wl_event_source_remove(demo.timer);
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
    return status;
}
