#ifdef __TINYC__
void *malloc(unsigned long);
void free(void *);
#ifndef NULL
#define NULL ((void *)0)
#endif
#else
#include <stdlib.h>
#endif
#include "han-font.h"
#include "../links/unicode.h"
#include "../tcc/lixin.h"

unsigned char *lx_load_font_atlas(const char *path, int bytes) {
    int fd = lx_open(path);
    if (fd < 0) return NULL;
    unsigned char *data = malloc(bytes);
    int offset = 0;
    while (data && offset < bytes) {
        int n = lx_read(fd, data + offset, bytes - offset);
        if (n <= 0) break;
        offset += n;
    }
    lx_close(fd);
    if (offset == bytes) return data;
    free(data);
    return NULL;
}

const unsigned char *lx_han_glyph(unsigned codepoint) {
    static unsigned char *font;
    static int attempted;
    int slot = han_slot(codepoint);
    if (slot < 0)
        return NULL;
    if (!attempted) {
        attempted = 1;
        font = lx_load_font_atlas("/src/links/han.font", 0x7300 * 32);
    }
    return font ? font + slot * 32 : NULL;
}

/* Load just the Unicode pages the UI encounters. Missing pages on older
 * installations fall back to the existing desktop and bitmap fonts. */
const unsigned char *lx_pingfang_glyph(unsigned codepoint, int scale) {
    static unsigned char *pages[0x73];
    static unsigned char attempted[0x73];
    static unsigned char alpha[32 * 32];
    static const char hex[] = "0123456789abcdef";
    int slot = han_slot(codepoint);
    if (slot < 0 || (scale != 1 && scale != 2))
        return NULL;
    int page = slot >> 8;
    if (!attempted[page]) {
        attempted[page] = 1;
        char path[] = "/lib/netsurf/pingfang/00.gray";
        const int digits = sizeof(path) - sizeof("00.gray");
        path[digits] = hex[(codepoint >> 12) & 15];
        path[digits + 1] = hex[(codepoint >> 8) & 15];
        pages[page] = lx_load_font_atlas(path, 32 + 256 * 640);
    }
    unsigned char *data = pages[page];
    int index = slot & 255;
    if (!data || !(data[index / 8] & (0x80 >> (index % 8))))
        return NULL;
    const unsigned char *packed = data + 32 + 640 * index + (scale == 2 ? 128 : 0);
    int size = 16 * scale;
    for (int i = 0; i < size * size; i += 2) {
        unsigned pair = *packed++;
        alpha[i] = (pair >> 4) * 17;
        alpha[i + 1] = (pair & 15) * 17;
    }
    return alpha;
}

int lx_latin_glyph(unsigned codepoint, int style, int scale, const unsigned char **mask) {
    static unsigned char *pages[8][5];
    static unsigned char attempted[8][5];
    static unsigned char alpha[40 * 32];
    static int selected;
    static unsigned char *cells;
    if (!selected) {
        selected = 1;
        cells = lx_load_font_atlas("/lib/fonts/console.gray", 95*22*44);
    }
    if (cells && style == 0 && codepoint >= 32 && codepoint < 127 && (scale == 1 || scale == 2)) {
        if (mask) {
            const unsigned char *cell = cells + (codepoint-32)*22*44;
            for (int y=0; y<16*scale; ++y)
                for (int x=0; x<20*scale; ++x) {
                    int col = x/scale-2;
                    alpha[y*20*scale+x] = col >= 0 && col < 9 ? cell[(3+y/scale)*44+col*22/9] : 0;
                }
            *mask = alpha;
        }
        return 9*scale;
    }
    unsigned high = codepoint >> 8;
    if ((high > 2 && high != 0x20 && high != 0x21) || (unsigned)style >= 8 ||
        (scale != 1 && scale != 2)) return 0;
    unsigned page = high <= 2 ? high : high - 0x20 + 3;
    if (!attempted[style][page]) {
        attempted[style][page] = 1;
        char path[] = "/lib/netsurf/latin/0-00.gray";
        char *name = path + sizeof("/lib/netsurf/latin/") - 1;
        name[0] += style;
        name[2] += high >> 4;
        name[3] += high & 15;
        pages[style][page] = lx_load_font_atlas(path, 512 + 256 * 800);
    }
    const unsigned char *data = pages[style][page];
    unsigned index = codepoint & 255;
    int advance = data ? data[256 * (scale - 1) + index] : 0;
    if (advance && mask) {
        const unsigned char *packed = data + 512 + 800*index + (scale == 2 ? 160 : 0);
        for (int i = 0; i < 320*scale*scale; i += 2) {
            unsigned pair = *packed++;
            alpha[i] = (pair >> 4) * 17;
            alpha[i+1] = (pair & 15) * 17;
        }
        *mask = alpha;
    }
    return advance;
}
