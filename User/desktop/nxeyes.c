/* Independent desktop window client inspired by the Nano-X mouse demo. */
#include "window-client.h"
#include "../console.h"
#include "../tcc/lixin.h"

__asm__(".section .text.start,\"ax\"\n.global _start\n_start: b eyes_main\n.previous");

static int pupil_offset(int delta, int distance, int limit) {
    return delta * limit / (distance > 24 ? distance : 24);
}

__attribute__((noreturn)) void eyes_main(void) {
    WindowFrame frame;
    WindowEvent event;
    int x = 64, y = 36;
    unsigned dark = 0;
    for (;;) {
        unsigned char *bytes = (unsigned char *)&event;
        for (unsigned offset = 0; offset < sizeof event;) {
            int n = lx_read(0, bytes + offset, sizeof event - offset);
            if (n <= 0) finish(offset != 0 || n < 0);
            offset += n;
        }
        if (event.type == WINDOW_CLIENT_POINTER) {
            x = event.x;
            y = event.y;
            dark = event.value != 0;
        } else if (event.type == WINDOW_CLIENT_KEY && event.value == 27) {
            finish(0);
        }
        for (int j = 0; j < WINDOW_CLIENT_HEIGHT; ++j)
            for (int i = 0; i < WINDOW_CLIENT_WIDTH; ++i)
                frame[j][i] = dark ? 0x310a : 0x29cd;
        for (int eye = 38; eye <= 90; eye += 52) {
            int dx = x - eye, dy = y - 36;
            int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            int px = pupil_offset(dx, distance, 11);
            int py = pupil_offset(dy, distance, 15);
            for (int j = -26; j <= 26; ++j) {
                for (int i = -21; i <= 21; ++i) {
                    if (i*i*676 + j*j*441 > 441*676) continue;
                    int a = i - px, b = j - py;
                    frame[36+j][eye+i] = a*a + b*b < 64 ? 0x18c5 : 0xffff;
                }
            }
        }
        bytes = (unsigned char *)frame;
        for (unsigned offset = 0; offset < sizeof frame;) {
            int n = lx_write(1, bytes + offset, sizeof frame - offset);
            if (n <= 0) finish(n < 0);
            offset += n;
        }
    }
}
