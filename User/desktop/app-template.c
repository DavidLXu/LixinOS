#include "/src/desktop/window-app.h"

static int enabled, held;

static int event(AppWindow *s, WindowEvent e) {
    int toggle = e.type == WINDOW_CLIENT_KEY && e.value == ' ';
    if (e.type == WINDOW_CLIENT_POINTER) {
        toggle = e.value && !held && app_hit(e, 8, 12, s->width-16, s->height-24);
        held = e.value;
    }
    if (toggle) enabled = !enabled;
    return toggle;
}

static void draw(AppWindow *s) {
    app_rect(s, 0, 0, s->width, s->height, app_rgb(22, 31, 43));
    app_rect(s, 8, 12, s->width-16, s->height-24,
             enabled ? app_rgb(43, 190, 137) : app_rgb(79, 110, 177));
    app_text(s, 16, (s->height-22)/2, enabled ? "ON" : "OFF", 0xffff);
}

int main(void) { return app_run(event, draw); }
