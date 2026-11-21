// Build inside LixinOS: tcc /src/desktop/counter.c -o /bin/counter.win
#include <lixin.h>
#include "window-client.h"

static WindowFrame pixels;

static void rectangle(int x, int y, int w, int h, unsigned color) {
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            pixels[j][i] = color;
}

static void digit(int x, int n) {
    static const unsigned mask[] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    static const int segments[][4] = {
        {3,0,16,3}, {19,3,3,14}, {19,20,3,14}, {3,34,16,3},
        {0,20,3,14}, {0,3,3,14}, {3,17,16,3}
    };
    for (int i = 0; i < 7; ++i) {
        const int *r = segments[i];
        rectangle(x+r[0], 18+r[1], r[2], r[3], mask[n] & (1 << i) ? 0xffbe : 0x1946);
    }
}

int main(void) {
    WindowEvent event;
    int count = 0, down = 0;
    for (;;) {
        for (unsigned offset = 0; offset < sizeof event;) {
            int n = lx_read(0, (char *)&event + offset, sizeof event - offset);
            if (n <= 0) return n < 0 || offset != 0;
            offset += n;
        }
        if (event.type == WINDOW_CLIENT_POINTER) {
            if (event.value && !down) count = (count + 1) % 100;
            down = event.value;
        } else if (event.type == WINDOW_CLIENT_KEY) {
            if (event.value == '+') count = (count + 1) % 100;
            if (event.value == '-') count = (count + 99) % 100;
            if (event.value == '0') count = 0;
        }
        rectangle(0, 0, WINDOW_CLIENT_WIDTH, WINDOW_CLIENT_HEIGHT, down ? 0x122a : 0x0967);
        digit(38, count / 10);
        digit(68, count % 10);
        for (unsigned offset = 0; offset < sizeof pixels;) {
            int n = lx_write(1, (char *)pixels + offset, sizeof pixels - offset);
            if (n <= 0) return n < 0;
            offset += n;
        }
    }
}
