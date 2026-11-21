#include <stddef.h>
#include "han.h"
#include "links/unicode.h"

int ns_han_width(uint32_t code) { return han_slot(code) < 0 ? 1 : 2; }

const uint8_t *ns_han_glyph(uint32_t code, int scale) {
    const uint8_t *mask = lx_pingfang_glyph(code, scale);
    if (mask) return mask;
    static uint8_t fallback[32 * 32];
    const uint8_t *bits = lx_han_glyph(code);
    if (!bits || (scale != 1 && scale != 2)) return NULL;
    int width = 16 * scale;
    for (int y = 0; y < width; ++y)
        for (int x = 0; x < width; ++x)
            fallback[y * width + x] =
                bits[(y / scale) * 2 + (x / scale) / 8] & (0x80 >> ((x / scale) % 8)) ? 255 : 0;
    return fallback;
}

int ns_char_advance(uint32_t code, int style, int scale) {
    int advance = lx_latin_glyph(code, style, scale, NULL);
    return advance ? advance : ns_han_width(code) * 8 * scale;
}
