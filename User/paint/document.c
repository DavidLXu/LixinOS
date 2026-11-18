// A portable SVG with editable object records. Reject foreign or modified SVG
// rather than discarding content that the editor cannot reconstruct.
#include "paint.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

const unsigned palette[COLORS] = {0x293344, 0xcb4c50, 0xdb863d, 0x448d63,
                                  0x3683bf, 0x8765ae, 0x148e94};
int add_point(Shape *s, Point p) {
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        Point *points = realloc(s->p, cap * sizeof(*points));
        if (!points)
            return 0;
        s->p = points;
        s->cap = cap;
    }
    s->p[s->n++] = p;
    return 1;
}
int append_shape(Drawing *d, Shape *s) {
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 16;
        Shape *shapes = realloc(d->s, cap * sizeof(*shapes));
        if (!shapes)
            return 0;
        d->s = shapes;
        d->cap = cap;
    }
    d->s[d->n++] = *s;
    *s = (Shape){0};
    return 1;
}
void arrow_head(Point a, Point b, int width, Point h[3]) {
    double dx = b.x - a.x, dy = b.y - a.y, len = hypot(dx, dy);
    double back = fmin(len, 9 + width * 2.5), side = back * .48;
    if (len) {
        dx /= len;
        dy /= len;
    }
    h[0] = b;
    h[1] = (Point){lround(b.x - back * dx - side * dy), lround(b.y - back * dy + side * dx)};
    h[2] = (Point){lround(b.x - back * dx + side * dy), lround(b.y - back * dy - side * dx)};
}
typedef struct {
    char *p;
    size_t len, cap;
    int failed;
} Text;
static void emit(Text *t, const char *fmt, ...) {
    if (t->failed)
        return;
    char line[320];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(line) || t->len + n > 1024 * 1024) {
        t->failed = 1;
        return;
    }
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? 2 * t->cap : 512;
        char *p = realloc(t->p, cap);
        if (!p) {
            t->failed = 1;
            return;
        }
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->len, line, n + 1);
    t->len += n;
}
static const char header[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 900 "
                             "480\">\n<rect width=\"900\" height=\"480\" fill=\"white\"/>\n";
char *make_svg(const Drawing *d, size_t *length) {
    Text t = {0};
    emit(&t, "%s", header);
    for (size_t i = 0; i < d->n && !t.failed; i++) {
        const Shape *s = &d->s[i];
        Point a = s->p[0], b = s->p[s->n - 1];
        unsigned color = palette[s->color];
        emit(&t, "<!--lx %d %d %d %u", s->tool, s->color, s->width, (unsigned)s->n);
        for (size_t j = 0; j < s->n; j++)
            emit(&t, " %d %d", s->p[j].x, s->p[j].y);
        emit(&t,
             " -->\n<g stroke=\"#%06x\" stroke-width=\"%d\" fill=\"none\" stroke-linecap=\"round\" "
             "stroke-linejoin=\"round\">",
             color, s->width);
        int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y, w = abs(b.x - a.x),
            h = abs(b.y - a.y);
        if (s->tool == RECT)
            emit(&t, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"/>", x, y, w, h);
        else if (s->tool == OVAL)
            emit(&t, "<ellipse cx=\"%.1f\" cy=\"%.1f\" rx=\"%.1f\" ry=\"%.1f\"/>", x + w * .5,
                 y + h * .5, w * .5, h * .5);
        else if (s->tool == PEN && s->n == 1)
            emit(&t, "<circle cx=\"%d\" cy=\"%d\" r=\"%.1f\" stroke=\"none\" fill=\"#%06x\"/>", a.x,
                 a.y, s->width * .5, color);
        else {
            emit(&t, "<path d=\"M%d %d", a.x, a.y);
            if (s->tool == CURVE)
                emit(&t, " Q%d %d %d %d", s->p[1].x, s->p[1].y, b.x, b.y);
            else
                for (size_t j = 1; j < s->n; j++)
                    emit(&t, " L%d %d", s->p[j].x, s->p[j].y);
            emit(&t, "\"/>");
            if (s->tool == ARROW) {
                Point h[3];
                arrow_head(a, b, s->width, h);
                emit(&t, "<polygon points=\"%d,%d %d,%d %d,%d\" fill=\"#%06x\" stroke=\"none\"/>",
                     h[0].x, h[0].y, h[1].x, h[1].y, h[2].x, h[2].y, color);
            }
        }
        emit(&t, "</g>\n");
    }
    emit(&t, "</svg>\n");
    if (t.failed) {
        free(t.p);
        return NULL;
    }
    *length = t.len;
    return t.p;
}
static int number(const char **p, int max) {
    if (*(*p)++ != ' ')
        return -1;
    char *end;
    long n = strtol(*p, &end, 10);
    if (end == *p || n < 0 || n > max)
        return -1;
    *p = end;
    return (int)n;
}
int load_svg(Drawing *d, const char *src, size_t length) {
    if (!length)
        return 1;
    if (strncmp(src, header, sizeof(header) - 1))
        return 0;
    const char *p = src + sizeof(header) - 1;
    while (!strncmp(p, "<!--lx", 6)) {
        p += 6;
        Shape s = {0};
        int tool = number(&p, TOOLS - 1);
        if (tool < 0)
            return 0;
        s.tool = tool;
        s.color = number(&p, COLORS - 1);
        if (s.color < 0)
            return 0;
        s.width = number(&p, 32);
        if (s.width < 1)
            return 0;
        int n = number(&p, 200000);
        if (n < 1 || (tool != PEN && n != (tool == CURVE ? 3 : 2)))
            return 0;
        for (int j = 0; j < n; j++) {
            int x = number(&p, ART_W - 1);
            if (x < 0)
                return 0;
            int y = number(&p, ART_H - 1);
            if (y < 0 || !add_point(&s, (Point){x, y}))
                return 0;
        }
        if (strncmp(p, " -->\n", 5) || !append_shape(d, &s))
            return 0;
        p = strchr(p + 5, '\n');
        if (!p)
            return 0;
        p++;
    }
    size_t size;
    char *canonical = make_svg(d, &size);
    int ok = canonical && size == length && !memcmp(canonical, src, size);
    free(canonical);
    return ok;
}
