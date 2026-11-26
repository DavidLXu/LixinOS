#ifndef LX_CELL_FONT_H
#define LX_CELL_FONT_H
#include "../links/unicode.h"
int lx_open(const char *);
int lx_read(int, void *, int);
int lx_close(int);
static inline int lx_cell_face(unsigned char *atlas) {
    int fd = lx_open("/lib/fonts/console.gray");
    if (fd >= 0) {
        unsigned char src[22 * 44];
        unsigned glyph;
        for (glyph = 0; glyph < 95; ++glyph) {
            unsigned n = 0;
            while (n < sizeof(src)) {
                int got = lx_read(fd, src + n, sizeof(src) - n);
                if (got <= 0) break;
                n += got;
            }
            if (n != sizeof(src)) break;
            for (unsigned y = 0; y < 22; ++y)
                for (unsigned x = 0; x < 9; ++x) {
                    unsigned i = 2*y*22 + x*22/9;
                    atlas[(glyph*22+y)*18+x] =
                        (src[i]+src[i+1]+src[i+22]+src[i+23])/4;
                }
        }
        lx_close(fd);
        if (glyph == 95) return 0;
    }
    return -1;
}

/* The factory Unifont atlas is read only when a wide glyph is first drawn. */
static inline const unsigned char *lx_gui_han_bitmap(int slot) {
    static unsigned char *atlas;
    static int tried;
    if (!tried) {
        tried = 1;
        int fd = lx_open("/src/links/han.font");
        if (fd >= 0) {
            extern void *malloc(unsigned long);
            extern void free(void *);
            unsigned size = 0x7300u * 32, n = 0;
            unsigned char *buf = malloc(size);
            while (buf && n < size) {
                int got = lx_read(fd, buf+n, size-n);
                if (got <= 0) break;
                n += got;
            }
            lx_close(fd);
            if (n == size) atlas = buf;
            else free(buf);
        }
    }
    return atlas ? atlas + slot * 32 : 0;
}
#endif
