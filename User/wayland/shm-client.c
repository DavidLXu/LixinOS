#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "demo-pattern.h"

struct objects {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    struct objects *objects = data;
    (void)version;
    if (!strcmp(interface, wl_compositor_interface.name))
        objects->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, wl_shm_interface.name))
        objects->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}

static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static void released(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    *(int *)data = 1;
}

static const struct wl_registry_listener registry_listener = {global, removed};
static const struct wl_buffer_listener buffer_listener = {released};

static struct wl_buffer *make_buffer(struct wl_shm *shm)
{
    int fd = memfd_create("wayland-demo", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, DEMO_BYTES) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = mmap(NULL, DEMO_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (unsigned y = 0; y < DEMO_HEIGHT; ++y)
        for (unsigned x = 0; x < DEMO_WIDTH; ++x)
            pixels[y * DEMO_WIDTH + x] = demo_pixel(x, y);
    munmap(pixels, DEMO_BYTES);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, DEMO_BYTES);
    close(fd);
    if (!pool) return NULL;
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, DEMO_WIDTH, DEMO_HEIGHT,
                                                          DEMO_WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return buffer;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    struct wl_display *display = wl_display_connect(argv[1]);
    if (!display) {
        perror("wayland client connect");
        return 1;
    }
    int seen = 0, status = 1;
    struct objects objects = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    struct wl_buffer *buffer = NULL;
    struct wl_surface *surface = NULL;
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &objects) < 0 ||
        wl_display_roundtrip(display) < 0 || !objects.compositor || !objects.shm)
        goto done;

    buffer = make_buffer(objects.shm);
    surface = wl_compositor_create_surface(objects.compositor);
    if (!buffer || !surface || wl_buffer_add_listener(buffer, &buffer_listener, &seen) < 0)
        goto done;
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, DEMO_WIDTH, DEMO_HEIGHT);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) >= 0 && seen) status = 0;

done:
    if (status) fprintf(stderr, "Wayland client did not complete the buffer roundtrip\n");
    if (buffer) wl_buffer_destroy(buffer);
    if (surface) wl_surface_destroy(surface);
    if (objects.shm) wl_shm_destroy(objects.shm);
    if (objects.compositor) wl_compositor_destroy(objects.compositor);
    if (registry) wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return status;
}
