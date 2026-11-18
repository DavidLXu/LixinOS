#pragma once
#include <stddef.h>
#include <stdint.h>
enum Tool { PEN, LINE, ARROW, CURVE, RECT, OVAL, TOOLS };
enum { ART_W = 900, ART_H = 480, COLORS = 7 };
typedef struct {
    int x, y;
} Point;
typedef struct {
    enum Tool tool;
    int color, width;
    Point *p;
    size_t n, cap;
} Shape;
typedef struct {
    Shape *s;
    size_t n, cap;
} Drawing;
extern const unsigned palette[COLORS];
int add_point(Shape *s, Point p);
int append_shape(Drawing *d, Shape *s);
int load_svg(Drawing *d, const char *src, size_t length);
char *make_svg(const Drawing *d, size_t *length);
void raster(const Shape *s, unsigned *pixels, int w, int h, double scale, int ox, int oy);
void arrow_head(Point a, Point b, int width, Point head[3]);
