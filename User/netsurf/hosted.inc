/* Adapter between the upstream framebuffer widgets and the LixinOS window manager.
 * Included by the framebuffer frontend so it can use the same widget tree as upstream. */
#include "../desktop/browser.h"
#include "netsurf/browser.h"
static unsigned *screen_pixels;
static int screen_stride, origin_x, origin_y, surface_w, surface_h;
static bool pressed;
static const char *initial_url;
static bool started;
void lixin_initial_url(const char *url) { initial_url = url; }
static int new_page(const char *address) {
    nsurl *url;
    struct browser_window *bw;
    if (nsurl_create(address, &url) != NSERROR_OK) return 0;
    nserror err = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
    nsurl_unref(url);
    return err == NSERROR_OK;
}
int desk_web_open(unsigned *pixels, int stride, int height, const char *cwd, int w, int h) {
    const char *address = initial_url ? initial_url : NETSURF_HOMEPAGE;
    initial_url = NULL;
    screen_pixels = pixels;
    screen_stride = stride;
    if (!started) {
        char *args[] = { "netsurf", "--enable_javascript=1", "-f", "ram", (char *)address, NULL };
        /* The window manager already scales logical pixels for Retina. */
        browser_set_dpi(96);
        if (lixin_core_open(5, args) != 0) return 0;
        started = true;
    } else if (!new_page(address)) return 0;
    surface_w = surface_h = 0;
    return 1;
}
const char *desk_web_title(void) {
    const char *title = window_list ? browser_window_get_title(window_list->bw) : NULL;
    return title && *title ? title : "NetSurf";
}
void desk_web_close(void) {
    pressed = false;
    while (window_list) browser_window_destroy(window_list->bw);
}
int desk_web_tick(void) {
    schedule_run();
    return fbtk_get_redraw_pending(fbtk);
}
void desk_web_draw(int x, int y, int w, int h) {
    origin_x = x;
    origin_y = y + 44;
    h -= 44;
    if (w != surface_w || h != surface_h) {
        gui_resize(fbtk, w, h);
        nsfb_get_geometry(fbtk_get_nsfb(fbtk), &surface_w, &surface_h, NULL);
    }
    fbtk_redraw(fbtk);
    uint8_t *src;
    int pitch;
    nsfb_get_buffer(fbtk_get_nsfb(fbtk), &src, &pitch);
    for (int row=0; row<h && row<surface_h; row++)
        memcpy(screen_pixels + (origin_y+row)*screen_stride+x, src+row*pitch,
               (w < surface_w ? w : surface_w)*sizeof(unsigned));
}
static void mouse_button(int button, bool down) {
    nsfb_event_t event = {.type = down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP};
    event.value.keycode = button;
    fbtk_click(fbtk, &event);
}
int desk_web_pointer(int x, int y, int down, int wheel) {
    fbtk_warp_pointer(fbtk, x-origin_x, y-origin_y, false);
    if (wheel) {
        int button = wheel > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
        mouse_button(button,true);
        mouse_button(button,false);
    }
    if (pressed != !!down) {
        pressed = !!down;
        mouse_button(NSFB_KEY_MOUSE_1, pressed);
    }
    return 1;
}
void desk_web_cancel_pointer(void) {
    if (pressed && window_list) mouse_button(NSFB_KEY_MOUSE_1,false);
    pressed = false;
}
int desk_web_key(int key) {
    if (key == 12 && window_list && window_list->url) {
        fbtk_set_text(window_list->url, "");
        fbtk_set_focus(window_list->url);
        return 1;
    }
    if (key == 27) return 0;
    nsfb_event_t event = {.type=NSFB_EVENT_KEY_DOWN};
    event.value.keycode = key == 127 ? NSFB_KEY_BACKSPACE : key;
    fbtk_input(fbtk,&event);
    event.type = NSFB_EVENT_KEY_UP;
    fbtk_input(fbtk,&event);
    return 1;
}
