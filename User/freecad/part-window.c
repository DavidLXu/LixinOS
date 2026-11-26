#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../desktop/window-app.h"
#include "part.h"

static double radius = 5, yaw = 0.55;
static int down;
static AppWindow *canvas;
static char status[96];

static void project(double x, double y, double z, int *sx, int *sy) {
    double scale = fmin(canvas->width / 61.0, (canvas->height - 84) / 46.0);
    if (scale < 0) scale = 0;
    x -= 18;
    y -= 12;
    *sx = canvas->width / 2 + (int)(scale * (x*cos(yaw) - y*sin(yaw)));
    *sy = (canvas->height + 15) / 2 + (int)(scale *
          ((x*sin(yaw) + y*cos(yaw)) * 0.55 - (z-4) * 0.84));
}

static void edge(double ax, double ay, double az, double bx, double by, double bz) {
    int x, y, end_x, end_y;
    project(ax, ay, az, &x, &y);
    project(bx, by, bz, &end_x, &end_y);
    int dx = end_x-x, dy = end_y-y, steps = abs(dx) + abs(dy);
    for (int i=0; i<=steps; ++i)
        app_rect(canvas, x + i*dx/(steps ? steps : 1), y + i*dy/(steps ? steps : 1), 2, 2, 0x966f);
}

static const char *buttons[] = {"Hole -", "Hole +", "Rotate", "Save"};
static void draw(AppWindow *s) {
    app_rect(s, 0, 0, s->width, s->height, 0x1125);
    for (int i=0; i<4; ++i) {
        int x = i*s->width/4;
        app_rect(s, x+1, 1, s->width/4-2, 28, 0x2949);
        app_text(s, x+5, 4, buttons[i], 0xffff);
    }
    canvas = s;
    part_edges(edge);
    canvas = NULL;
    app_text(s, 6, s->height-25, status, 0xe75a);
}
static void dimensions(void) {
    snprintf(status, sizeof status, "Hole R%.1f mm   Volume %.1f mm3", radius, part_volume());
}

static int event(AppWindow *s, WindowEvent e) {
    int action;
    if (e.type == WINDOW_CLIENT_POINTER) {
        int was_down = down;
        down = (e.value & 1) != 0;
        if (!down || was_down || e.y<0 || e.y>=30 || e.x<0 || e.x>=s->width) return 0;
        action = e.x*4/s->width;
    } else if (e.type == WINDOW_CLIENT_KEY) {
        const char *keys = "-+rs";
        const char *key = e.value>0 && e.value<128 ? strchr(keys,e.value) : NULL;
        if (!key) return 0;
        action = (int)(key-keys);
    } else return 0;
    if (action < 2) {
        double next = radius + (action ? 0.5 : -0.5);
        if (part_plate(next)) radius = next;
        dimensions();
    } else if (action == 2) {
        yaw += 0.25;
    } else {
        char path[40];
        for (int i=1; i<=999; ++i) {
            snprintf(path, sizeof path, "plate-%03d.brep", i);
            if (lx_stat(path) != LX_MISSING) continue;
            if (part_save(path)) snprintf(status, sizeof status, "Saved %s", path);
            else snprintf(status, sizeof status, "Could not save %s", path);
            return 1;
        }
        snprintf(status,sizeof status,"No free plate filename");
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc==3 && !strcmp(argv[1],"--inspect")) {
        if (!part_read(argv[2])) return 1;
        printf("volume %.6f mm3\n",part_volume());
        return 0;
    }
    if (!part_plate(radius)) return 1;
    if (argc==3 && !strcmp(argv[1],"--create")) return part_save(argv[2]) ? 0 : 1;
    dimensions();
    return app_run3(event, draw);
}
