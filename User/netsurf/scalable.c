// Rasterize trusted, locally generated CoreText outlines at the requested em size.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../tcc/lixin.h"
#include "links/unicode.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "han.h"

struct outline_page { uint32_t *data; size_t bytes; int tried; };
static struct outline_page pages[10][256];

static const int16_t *lookup(unsigned code, int style) {
    if (code > 0xffff) return NULL;
    unsigned high = code >> 8;
    if (code >= 0xac00 && code <= 0xd7a3) style = 9;
    else if (han_slot(code) >= 0) style = 8;
    else if (high > 2 && high != 0x20 && high != 0x21) return NULL;
    struct outline_page *p = &pages[style][high];
    if (!p->tried) {
        p->tried = 1;
        char path[48];
        snprintf(path, sizeof(path), "/lib/netsurf/outlines/%d-%02x.out", style, high);
        long size = lx_stat(path);
        if (size < 1024 || size > 2048*1024) return NULL;
        int fd = lx_open(path);
        if (fd < 0) return NULL;
        p->data = malloc(size);
        while (p->data && p->bytes < (size_t)size) {
            int n = lx_read(fd, (char *)p->data + p->bytes, size - p->bytes);
            if (n <= 0) break;
            p->bytes += n;
        }
        lx_close(fd);
        if (p->bytes != (size_t)size) { free(p->data); p->data = NULL; }
    }
    if (!p->data) return NULL;
    uint32_t off = p->data[code & 255];
    if (off < 1024 || off % 2 || off > p->bytes - 4) return NULL;
    const int16_t *g = (const int16_t *)((const char *)p->data + off);
    return g[0] > 0 && g[1] >= 0 && (size_t)g[1]*2 <= p->bytes-off-4 ? g : NULL;
}

int ns_advance(unsigned code, int style, int px) {
    if (code >= 0xac00 && code <= 0xd7a3) return px;
    if (px == 16 || px == 32) return ns_char_advance(code, style, px/16);
    const int16_t *g = lookup(code, style);
    if (g) return (g[0]*px + 512)/1024;
    return (ns_char_advance(code, style, 2)*px + 16)/32;
}

const unsigned char *ns_outline_mask(unsigned code, int style, int px, int *stride) {
    static struct entry { unsigned code; int style, px; unsigned char *mask; } cache[128];
    struct entry *e = &cache[(code*31u + style*17u + px) % 128];
    *stride = px + (px+1)/2;
    if (e->mask && e->code == code && e->style == style && e->px == px) return e->mask;
    const int16_t *g = lookup(code, style);
    if (!g) return NULL;
    int words = g[1], n = 0;
    stbtt_vertex *v = words ? calloc(words/3+1, sizeof(*v)) : NULL;
    if (words && !v) return NULL;
    for (int i = 2; i < words+2;) {
        int type = g[i++];
        int count = type == STBTT_vcubic ? 6 : type == STBTT_vcurve ? 4 : 2;
        if (type < 1 || type > 4 || i+count > words+2) { free(v); return NULL; }
        v[n].type = type; v[n].x = g[i]; v[n].y = g[i+1];
        if (count >= 4) { v[n].cx = g[i+2]; v[n].cy = g[i+3]; }
        if (count == 6) { v[n].cx1 = g[i+4]; v[n].cy1 = g[i+5]; }
        i += count; n++;
    }
    unsigned char *mask = calloc(*stride, px);
    if (!mask) { free(v); return NULL; }
    stbtt__bitmap bitmap = {*stride, px, *stride, mask};
    if (n) stbtt_Rasterize(&bitmap, 0.25f, v, n, px/1024.f, px/1024.f, 0, 0,
                           -(px/8), -(px-px/8), 1, NULL);
    free(v);
    free(e->mask);
    *e = (struct entry){code,style,px,mask};
    return mask;
}

static int listed(unsigned code, const char *s) {
    while (*s) if (utf8_next(&s) == code) return 1;
    return 0;
}

int ns_break_between(unsigned before, unsigned after) {
    if (!before || (han_slot(before) < 0 && han_slot(after) < 0)) return 0;
    return !listed(before, "（〔［｛〈《「『【〖〘〚‘“([{") &&
           !listed(after, "，。、；：？！）〕］｝〉》」』】〗〙〛’”…％‰℃!?%,.:;)]}");
}

#include "shaping.inc"
