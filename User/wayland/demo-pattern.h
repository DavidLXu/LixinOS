#pragma once
#include <stdint.h>

enum { DEMO_WIDTH = 320, DEMO_HEIGHT = 200, DEMO_BYTES = DEMO_WIDTH * DEMO_HEIGHT * 4 };

static inline uint32_t demo_pixel(unsigned x, unsigned y)
{
    if (y < 36) return 0x182e41;
    if (x >= 26 && x < 294 && y >= 64 && y < 172) {
        if (y < 124) return x < 160 ? 0x42bba1 : 0x3984d2;
        return x < 160 ? 0xeed796 : 0xc482ad;
    }
    return 0xe8edf2;
}
