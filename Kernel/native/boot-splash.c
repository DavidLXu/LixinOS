/* Shared by both graphical kernels. The boot CPU paints directly to scanout
 * before the filesystem, fonts and scheduler are available. */
#include "native.h"

extern u64 smp_ticks(void), smp_frequency(void);
static u32 *surface;
static unsigned pitch, left, top;
static u64 start, period, last_frame;
enum { BACKGROUND = 0x171b20, INK = 0xe4efe9, TEAL = 0x75d6c6,
       TRACK = 0x30434a, SCALE = 6, WIDTH = 41 * SCALE };

static void rect(unsigned x, unsigned y, unsigned w, unsigned h, u32 color) {
    for (unsigned row = 0; row < h; row++) {
        u32 *p = surface + (top + y + row) * pitch + left + x;
        for (unsigned col = 0; col < w; col++) p[col] = color;
    }
}

void boot_splash_tick(void) {
    if (!surface) return;
    u64 frame = (smp_ticks() - start) / period;
    if (frame == last_frame) return;
    last_frame = frame;
    /* A travelling light indicates activity, independent of disk capacity. */
    unsigned phase = frame % 80;
    unsigned x = (phase <= 40 ? phase : 80 - phase) * (WIDTH - 54) / 40;
    rect(0, 66, WIDTH, 4, TRACK);
    rect(x, 66, 54, 4, TEAL);
    gpu_damage(left, top + 66, WIDTH, 4);
    gpu_refresh();
}

void boot_splash_begin(u32 *fb, unsigned stride, unsigned w, unsigned h) {
    static const u8 word[7][7] = {
        {16,16,16,16,16,16,31}, {4,0,12,4,4,4,14},
        {0,0,17,10,4,10,17}, {4,0,12,4,4,4,14},
        {0,0,22,25,17,17,17}, {14,17,17,17,17,17,14},
        {15,16,16,14,1,1,30}
    };
    if (w < WIDTH || h < 70) return;
    surface = fb;
    pitch = stride;
    left = (w - WIDTH) / 2;
    top = (h - 70) / 2;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) fb[y * stride + x] = BACKGROUND;
    for (unsigned i = 0; i < 7; i++)
        for (unsigned y = 0; y < 7; y++)
            for (unsigned x = 0; x < 5; x++)
                if (word[i][y] & (16 >> x))
                    rect((i * 6 + x) * SCALE, y * SCALE, SCALE, SCALE, i < 5 ? INK : TEAL);
    start = smp_ticks();
    period = smp_frequency() / 30;
    if (!period) period = 1;
    last_frame = (u64)-1;
    gpu_damage(0, 0, w, h);
    boot_splash_tick();
}

void boot_splash_end(void) {
    if (!surface) return;
    rect(0, 0, WIDTH, 70, BACKGROUND);
    gpu_damage(left, top, WIDTH, 70);
    surface = 0;
}
