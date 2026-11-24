#ifndef LIXINOS_WAYLAND_SOURCE_H
#define LIXINOS_WAYLAND_SOURCE_H

#include <glib.h>

struct wl_display;

GSource *lx_wayland_source_new(struct wl_display *display);

#endif
