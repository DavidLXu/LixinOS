#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "i_system.h"
#include "../console.h"
#include "../gui/lixin-gui.h"

#include <stdio.h>
#include <string.h>

static struct lx_canvas canvas;
static unsigned viewport_x, viewport_y, width, height;
static unsigned *columns;
static uint32_t renewed[256];
static unsigned char held[256];
static int pending = -1, mouse_down;

void DG_Init(void) {}
void DG_SetWindowTitle(const char *title) { (void)title; }
void DG_SleepMs(uint32_t ms) { console_call(43, ms); }
uint32_t DG_GetTicksMs(void) { return (uint32_t)console_call(38, 0); }

static void update_canvas(struct lx_canvas next)
{
    canvas = next;
    width = canvas.width;
    height = canvas.height;
    if (width * 3 > height * 4) width = height * 4 / 3;
    else height = width * 3 / 4;
    viewport_x = (canvas.width - width) / 2;
    viewport_y = (canvas.height - height) / 2;
    free(columns);
    columns = malloc(width * sizeof *columns);
    if (!columns) I_Error("Unable to allocate the display scale table");
    for (unsigned x = 0; x < width; ++x) columns[x] = x * DOOMGENERIC_RESX / width;
    memset(canvas.pixels, 0, (size_t)canvas.width * canvas.height * sizeof *canvas.pixels);
    lx_gui_present(&canvas);
}

void DG_DrawFrame(void)
{
    struct lx_canvas next;
    if (lx_gui_open(&next) != 0) return;
    if (next.width != canvas.width || next.height != canvas.height) update_canvas(next);
    for (unsigned y = 0; y < height; ++y) {
        const pixel_t *row = DG_ScreenBuffer + y * DOOMGENERIC_RESY / height * DOOMGENERIC_RESX;
        unsigned *out = canvas.pixels + (viewport_y + y) * canvas.width + viewport_x;
        for (unsigned x = 0; x < width; ++x) out[x] = row[columns[x]];
    }
    lx_gui_present(&canvas);
}

static int read_key(void)
{
    int byte = pending;
    pending = -1;
    if (byte < 0) byte = lx_timed_byte(0);
    if (byte == 27) {
        int prefix = lx_timed_byte(12);
        if (prefix == '[' || prefix == 'O') {
            int next = lx_timed_byte(12);
            static const unsigned char arrows[] = {KEY_UPARROW, KEY_DOWNARROW, KEY_RIGHTARROW, KEY_LEFTARROW};
            if (next >= 'A' && next <= 'D') return arrows[next - 'A'];
            while (next >= 0 && next < 0x40) next = lx_timed_byte(12);
            return -1;
        }
        pending = prefix;
        return KEY_ESCAPE;
    }
    if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    if (menuactive && byte >= 32 && byte < 127) return byte;
    switch (byte) {
    case 17: I_Quit(); return -1;
    case 'w': return KEY_UPARROW;
    case 's': return KEY_DOWNARROW;
    case 'a': return KEY_STRAFE_L;
    case 'd': return KEY_STRAFE_R;
    case 'f': return KEY_FIRE;
    case ' ': return KEY_USE;
    case '\r': case '\n': return KEY_ENTER;
    case '\b': return KEY_BACKSPACE;
    default: return byte > 0 && byte < 128 ? byte : -1;
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    struct lx_pointer pointer;
    const uint32_t now = DG_GetTicksMs();
    if (lx_gui_decode_pointer(&pointer, console_call(21, 8))) {
        int down = pointer.inside && !pointer.cancelled && pointer.down;
        if (down != mouse_down) {
            mouse_down = down;
            if (!held[KEY_FIRE]) {
                *key = KEY_FIRE;
                *pressed = down;
                return 1;
            }
        }
    }
    for (unsigned i = 1; i < sizeof held; ++i) {
        if (!held[i] || now - renewed[i] < 155 || (i == KEY_FIRE && mouse_down)) continue;
        held[i] = 0;
        *pressed = 0;
        *key = i;
        return 1;
    }
    int value = read_key();
    if (value < 0) return 0;
    held[value] = 1;
    renewed[value] = now;
    *pressed = 1;
    *key = value;
    return 1;
}

int main(int argc, char **argv)
{
    struct lx_canvas next;
    int has_iwad = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-iwad")) has_iwad = 1;
        if (!strcmp(argv[i], "-gfxmode") || argv[i][0] == '@') {
            fprintf(stderr, "doom: this port uses a fixed RGB framebuffer and direct command-line options\n");
            return 2;
        }
    }
    if (lx_gui_open(&next) != 0) {
        fprintf(stderr, "doom: run this game in the graphical LixinOS shell (--window)\n");
        return 1;
    }
    char **args = calloc((size_t)argc + 3, sizeof *args);
    if (!args) return 1;
    memcpy(args, argv, (size_t)argc * sizeof *args);
    if (!has_iwad) {
        args[argc++] = "-iwad";
        args[argc++] = "/lib/doom/freedoom2.wad";
    }
    doomgeneric_Create(argc, args);
    for (;;) doomgeneric_Tick();
}
