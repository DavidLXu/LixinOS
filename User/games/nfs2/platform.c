#include <SDL.h>
#include "../../gui/lixin-gui.h"
#include "../../console.h"
#include <unistd.h>
#include <time.h>

static Uint32 expiry[SDL_NUM_SCANCODES];
extern int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode code);
extern void SDL_SetKeyboardFocus(SDL_Window *window);

int nfs_open_canvas(void) {
    struct lx_canvas canvas;
    return lx_gui_open(&canvas);
}

void nfs_input(void) {
    static const SDL_Scancode nav[] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_SPACE
    };
    static unsigned held;
    SDL_Window *window = SDL_GetWindowFromID(1);
    if (window && SDL_GetKeyboardFocus() != window) SDL_SetKeyboardFocus(window);
    const Uint32 now = SDL_GetTicks();
    for (int i = 1; i < SDL_NUM_SCANCODES; ++i) {
        if (expiry[i] && (Sint32)(now - expiry[i]) >= 0) {
            SDL_SendKeyboardKey(SDL_RELEASED, (SDL_Scancode)i);
            expiry[i] = 0;
        }
    }
    static int pending = -1;
    for (unsigned n = 0; n < 24; ++n) {
        int key = pending >= 0 ? pending : lx_timed_byte(0);
        pending = -1;
        if (key < 0 || key > 255) break;
        if (key == 17) _exit(0);
        if (key == 27) {
            int next = lx_timed_byte(8);
            if (next == '[' || next == 'O') {
                static const int arrows[] = {SDLK_UP, SDLK_DOWN, SDLK_RIGHT, SDLK_LEFT};
                next = lx_timed_byte(8);
                if (next < 'A' || next > 'D') continue;
                key = arrows[next - 'A'];
            } else if (next >= 0 && next < 256) pending = next;
        }
        if (key == '\n') key = SDLK_RETURN;
        if (key == 127) key = SDLK_BACKSPACE;
        SDL_Scancode code = SDL_GetScancodeFromKey(key);
        if (!code) continue;
        if (!expiry[code]) SDL_SendKeyboardKey(SDL_PRESSED, code);
        expiry[code] = now + 180;
    }
    long state = console_call(19, 2);
    if (state < 0) state = 0;
    for (unsigned i = 0; i < sizeof(nav)/sizeof(*nav); ++i) {
        unsigned bit = 1u << i;
        if ((state ^ held) & bit) {
            SDL_SendKeyboardKey(state & bit ? SDL_PRESSED : SDL_RELEASED, nav[i]);
            expiry[nav[i]] = 0;
        } else if (state & bit) expiry[nav[i]] = 0;
    }
    if ((unsigned)state != held) SDL_Log("nfs2: held navigation %ld", state);
    held = (unsigned)state;
}

void nfs_present(const void *pixels, unsigned w, unsigned h, unsigned depth, const unsigned *palette) {
    struct lx_canvas canvas;
    if (lx_gui_open(&canvas)) return;

    unsigned dw = canvas.width, dh = dw*h/w;
    if (dh > canvas.height) { dh = canvas.height; dw = dh*w/h; }
    const unsigned x0 = (canvas.width-dw)/2, y0 = (canvas.height-dh)/2;
    for (unsigned y = 0; y < dh; ++y) {
        unsigned row = y*h/dh*w;
        unsigned *dest = canvas.pixels + (y+y0)*canvas.width + x0;
        for (unsigned x = 0; x < dw; ++x) {
            unsigned pos = row + x*w/dw;
            if (depth == 8) dest[x] = palette[((const Uint8 *)pixels)[pos]];
            else {
                unsigned c = ((const Uint16 *)pixels)[pos];
                dest[x] = ((c & 0xf800) << 8) | ((c & 0x7e0) << 5) | ((c & 0x1f) << 3);
            }
        }
    }
    lx_gui_present(&canvas);
}

/* The guest monotonic clock has microsecond resolution. SDL's Unix backend
   probes CLOCK_MONOTONIC_RAW and otherwise falls back to whole wall seconds. */
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *ts) {
    return __real_clock_gettime(id == CLOCK_MONOTONIC_RAW ? CLOCK_MONOTONIC : id, ts);
}
