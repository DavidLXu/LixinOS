#include <stddef.h>
#include "device.h"
#include "fb.h"
#include "genfont.h"
#include "genmem.h"
#include "gui/lixin-gui.h"

static struct lx_canvas canvas;
static unsigned first_dirty_row, end_dirty_row;

static PSD open_screen(PSD screen) {
    if (lx_gui_open(&canvas) < 0)
        return NULL;
    if (!gen_initpsd(screen, MWPF_TRUECOLORARGB, canvas.width, canvas.height, PSF_SCREEN))
        return NULL;
    screen->addr = (unsigned char *)canvas.pixels;
    first_dirty_row = canvas.height;
    end_dirty_row = 0;
    return screen;
}

static void close_screen(PSD screen) {
    (void)screen;
}

static void set_palette(PSD screen, int first, int count, MWPALENTRY *entries) {
    (void)screen;
    (void)first;
    (void)count;
    (void)entries;
}

static void update_screen(PSD screen, MWCOORD x, MWCOORD y, MWCOORD w, MWCOORD h) {
    (void)screen;
    (void)x;
    (void)w;
    if (h <= 0 || y >= (int)canvas.height || y + h <= 0)
        return;
    unsigned first = y < 0 ? 0 : (unsigned)y;
    unsigned end = y + h > (int)canvas.height ? canvas.height : (unsigned)(y + h);
    if (first < first_dirty_row)
        first_dirty_row = first;
    if (end > end_dirty_row)
        end_dirty_row = end;
}

int lx_nx_flush(void) {
    if (first_dirty_row >= end_dirty_row)
        return 0;
    unsigned row = first_dirty_row, height = end_dirty_row - row;
    first_dirty_row = canvas.height;
    end_dirty_row = 0;
    return lx_canvas_present(canvas.pixels + row * canvas.width, 0, row, canvas.width, height);
}

SCREENDEVICE scrdev = {
    .builtin_fonts = gen_fonts,
    .Open = open_screen,
    .Close = close_screen,
    .SetPalette = set_palette,
    .GetScreenInfo = gen_getscreeninfo,
    .AllocateMemGC = gen_allocatememgc,
    .MapMemGC = gen_mapmemgc,
    .FreeMemGC = gen_freememgc,
    .SetPortrait = gen_setportrait,
    .Update = update_screen,
};
