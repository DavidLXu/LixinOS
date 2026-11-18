#include "paint.h"
#include <math.h>
#include <stdlib.h>
typedef struct {
    unsigned *px, color;
    int w, h, ox, oy;
    double scale, r;
} Surface;
static void dot(Surface *v, double x, double y) {
    int radius = (int)ceil(v->r);
    int cx = lround(x), cy = lround(y);
    for (int j = cy - radius; j <= cy + radius; j++) {
        if (j < 0 || j >= v->h)
            continue;
        for (int i = cx - radius; i <= cx + radius; i++) {
            if (i < 0 || i >= v->w)
                continue;
            double dx = i - x, dy = j - y;
            if (dx * dx + dy * dy <= v->r * v->r)
                v->px[j * v->w + i] = v->color;
        }
    }
}
static void line(Surface *v, Point a, Point b) {
    double x = v->ox + a.x * v->scale, y = v->oy + a.y * v->scale;
    double dx = (b.x - a.x) * v->scale, dy = (b.y - a.y) * v->scale;
    int n = ceil(fmax(fabs(dx), fabs(dy)));
    if (n < 1)
        n = 1;
    for (int i = 0; i <= n; i++)
        dot(v, x + dx * i / n, y + dy * i / n);
}
void raster(const Shape *s, unsigned *pixels, int w, int h, double scale, int ox, int oy) {
    if (!s->n)
        return;
    Surface v = {pixels, palette[s->color], w, h, ox, oy, scale, fmax(.8, s->width * scale * .5)};
    Point a = s->p[0], b = s->p[s->n - 1], prev = a;
    if (s->tool == RECT) {
        Point c = {a.x, b.y}, d = {b.x, a.y};
        line(&v, a, c);
        line(&v, c, b);
        line(&v, b, d);
        line(&v, d, a);
    } else if (s->tool == OVAL || s->tool == CURVE) {
        for (int i = 0; i <= 120; i++) {
            double t = i / 120., u = 1 - t;
            Point p;
            if (s->tool == OVAL) {
                double r = t * 6.283185307179586;
                p = (Point){lround((a.x + b.x + (b.x - a.x) * cos(r)) * .5),
                            lround((a.y + b.y + (b.y - a.y) * sin(r)) * .5)};
            } else {
                Point c = s->p[1];
                p = (Point){lround(u * u * a.x + 2 * u * t * c.x + t * t * b.x),
                            lround(u * u * a.y + 2 * u * t * c.y + t * t * b.y)};
            }
            if (i)
                line(&v, prev, p);
            prev = p;
        }
    } else {
        for (size_t j = 0; j < s->n; j++) {
            line(&v, prev, s->p[j]);
            prev = s->p[j];
        }
        if (s->tool == ARROW) {
            Point h[3];
            arrow_head(a, b, s->width, h);
            v.r = .8;
            int n = abs(h[1].x - h[2].x) + abs(h[1].y - h[2].y) + 1;
            for (int i = 0; i <= n; i++) {
                Point p = {h[1].x + (h[2].x - h[1].x) * i / n, h[1].y + (h[2].y - h[1].y) * i / n};
                line(&v, b, p);
            }
        }
    }
}
