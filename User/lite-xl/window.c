#include "window.h"
#include "../gui/shared-clipboard.h"

#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "lauxlib.h"
#include "renwindow.h"
#include "../console.h"
#include "../desktop/window-client.h"

static int width, height, pointer_x, pointer_y, pointer_down;
static Uint64 last_click, escape_started;
static int click_x, click_y, clicks;
static bool first_event = true, full_frame = true;
static SDL_Rect damage;
static unsigned short *packed_pixels;
static size_t packed_capacity;
static unsigned short *encoded_pixels;
static size_t encoded_capacity;
static char clipboard[WINDOW_CLIENT_CLIPBOARD_MAX];
static unsigned copied;
static char text[5];
static int text_size, text_needed;
static char escape[16];
static unsigned escaped;
enum { MOUSE_NONE, MOUSE_MOTION, MOUSE_BUTTON, MOUSE_WHEEL };
typedef struct {
    const char *type;
    SDL_Keycode key;
    char text[5];
    size_t paste_size;
    int mouse, x, y, dx, dy, clicks;
} Input;
static Input input[8];
static unsigned input_count, input_index;
static char pasted[WINDOW_CLIENT_CLIPBOARD_MAX];

static bool transfer(int fd, void *data, size_t size, bool output)
{
    char *p = data;
    while (size) {
        if (output && fd == STDOUT_FILENO) {
            struct pollfd ready = {.fd = fd, .events = POLLOUT};
            if (poll(&ready, 1, -1) < 0) {
                if (errno == EINTR) continue;
                return false;
            }
        }
        ssize_t n = output ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static bool configure(WindowEvent event)
{
    if (event.x < 1 || event.y < 1 || event.x > 2048 || event.y > 1152)
        return false;
    width = event.x;
    height = event.y;
    full_frame = true;
    damage = (SDL_Rect){0};
    return true;
}

bool lxl_boot(int *w, int *h)
{
    WindowEvent event;
    struct winsize tty;
    if (ioctl(0, TIOCGWINSZ, &tty) == 0 || !transfer(0, &event, sizeof event, false) ||
        event.type != WINDOW_CLIENT_CONFIGURE || !configure(event))
        return false;
    console_call(57, 0);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    *w = width;
    *h = height;
    return true;
}

static void key_event(const char *type, SDL_Keycode key)
{
    if (input_count < sizeof input / sizeof *input)
        input[input_count++] = (Input){.type = type, .key = key};
}

static void keystroke(SDL_Keycode key, bool ctrl)
{
    if (ctrl)
        key_event("keypressed", SDLK_LCTRL);
    key_event("keypressed", key);
    key_event("keyreleased", key);
    if (ctrl)
        key_event("keyreleased", SDLK_LCTRL);
}

static void text_byte(unsigned char byte)
{
    if ((byte & 0xc0) != 0x80) {
        text_size = 0;
        if (byte < 128) text_needed = 1;
        else if (byte >= 0xc2 && byte <= 0xdf) text_needed = 2;
        else if (byte >= 0xe0 && byte <= 0xef) text_needed = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) text_needed = 4;
        else text_needed = 0;
    }
    if (!text_needed || text_size >= text_needed)
        return;
    text[text_size++] = byte;
    if (text_size == text_needed) {
        text[text_size] = 0;
        if (input_count < sizeof input / sizeof *input) {
            Input *event = &input[input_count++];
            *event = (Input){.type = "textinput"};
            memcpy(event->text, text, sizeof text);
        }
        text_needed = text_size = 0;
    }
}

static void escape_key(void)
{
    static const struct { const char *sequence; SDL_Keycode key; } keys[] = {
        {"[A", SDLK_UP}, {"[B", SDLK_DOWN}, {"[C", SDLK_RIGHT}, {"[D", SDLK_LEFT},
        {"OA", SDLK_UP}, {"OB", SDLK_DOWN}, {"OC", SDLK_RIGHT}, {"OD", SDLK_LEFT},
        {"[H", SDLK_HOME}, {"[F", SDLK_END}, {"OH", SDLK_HOME}, {"OF", SDLK_END},
        {"[1~", SDLK_HOME}, {"[4~", SDLK_END}, {"[7~", SDLK_HOME}, {"[8~", SDLK_END},
        {"[2~", SDLK_INSERT}, {"[3~", SDLK_DELETE}, {"[5~", SDLK_PAGEUP}, {"[6~", SDLK_PAGEDOWN}
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (!strcmp(keys[i].sequence, escape)) {
            keystroke(keys[i].key, false);
            break;
        }
    }
}

static void keyboard(unsigned char byte)
{
    if (escaped) {
        if (escaped > 1 || byte == '[' || byte == 'O') {
            if (escaped >= sizeof escape) {
                escaped = 0;
                return;
            }
            escape[escaped++ - 1] = byte;
            escape[escaped - 1] = 0;
            if (escaped > 2 && byte >= 0x40 && byte <= 0x7e) {
                escape_key();
                escaped = 0;
            }
            return;
        }
        escaped = 0;
        keystroke(SDLK_ESCAPE, false);
    }
    switch (byte) {
    case 27:
        escaped = 1;
        escape_started = SDL_GetTicks();
        break;
    case 8:
    case 127: keystroke(SDLK_BACKSPACE, false); break;
    case 9: keystroke(SDLK_TAB, false); break;
    case 10:
    case 13: keystroke(SDLK_RETURN, false); break;
    default:
        if (byte >= 32)
            text_byte(byte);
        else if (byte >= 1 && byte <= 26)
            keystroke(SDLK_A + byte - 1, true);
    }
}

static void pointer(WindowEvent e)
{
    if (pointer_x != e.x || pointer_y != e.y) {
        input[input_count++] = (Input){.type = "mousemoved", .mouse = MOUSE_MOTION,
            .x = e.x, .y = e.y, .dx = e.x - pointer_x, .dy = e.y - pointer_y};
        pointer_x = e.x;
        pointer_y = e.y;
    }
    if (pointer_down == e.value)
        return;
    pointer_down = e.value;
    if (pointer_down) {
        Uint64 now = SDL_GetTicks();
        if (now - last_click <= 350 && abs(e.x - click_x) < 5 && abs(e.y - click_y) < 5)
            clicks = clicks < 3 ? clicks + 1 : 1;
        else
            clicks = 1;
        last_click = now;
        click_x = e.x;
        click_y = e.y;
    }
    input[input_count++] = (Input){.type = pointer_down ? "mousepressed" : "mousereleased",
        .mouse = MOUSE_BUTTON, .x = e.x, .y = e.y, .clicks = pointer_down ? clicks : 0};
}

static int next_event(lua_State *L)
{
    input_index = input_count = 0;
    if (first_event) {
        first_event = false;
        lua_pushboolean(L, true);
        return 1;
    }
    WindowEvent e;
    bool ok = transfer(0, &e, sizeof e, false);
    if (ok) {
        switch (e.type) {
        case WINDOW_CLIENT_CONFIGURE:
            ok = configure(e) && SDL_SetWindowSize(window_renderer.window, width, height);
            if (ok) {
                SDL_Event event = {.type = SDL_EVENT_WINDOW_EXPOSED};
                SDL_PushEvent(&event);
            }
            break;
        case WINDOW_CLIENT_KEY:
            keyboard(e.value);
            break;
        case WINDOW_CLIENT_POINTER:
            pointer(e);
            break;
        case WINDOW_CLIENT_WHEEL:
            input[input_count++] = (Input){.type = "mousewheel", .mouse = MOUSE_WHEEL, .dy = e.value};
            break;
        case WINDOW_CLIENT_CLOSE: {
            input[input_count++] = (Input){.type = "quit"};
            break;
        }
        case WINDOW_CLIENT_PASTE:
            ok = e.value > 0 && (size_t)e.value <= sizeof pasted &&
                 transfer(0, pasted, e.value, false);
            if (ok)
                input[input_count++] = (Input){.type = "textinput", .paste_size = e.value};
            break;
        case WINDOW_CLIENT_TIMER:
            if (escaped && SDL_GetTicks() - escape_started >= 45) {
                if (escaped == 1)
                    keystroke(SDLK_ESCAPE, false);
                escaped = 0;
            }
            break;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

int lxl_poll_input(lua_State *L)
{
    if (input_index == input_count)
        return 0;
    const Input *event = &input[input_index++];
    lua_pushstring(L, event->type);
    switch (event->mouse) {
    case MOUSE_MOTION:
        lua_pushinteger(L, event->x);
        lua_pushinteger(L, event->y);
        lua_pushinteger(L, event->dx);
        lua_pushinteger(L, event->dy);
        return 5;
    case MOUSE_BUTTON:
        lua_pushliteral(L, "left");
        lua_pushinteger(L, event->x);
        lua_pushinteger(L, event->y);
        if (!event->clicks) return 4;
        lua_pushinteger(L, event->clicks);
        return 5;
    case MOUSE_WHEEL:
        lua_pushinteger(L, event->dy);
        lua_pushinteger(L, 0);
        return 3;
    }
    if (event->paste_size) {
        lua_pushlstring(L, pasted, event->paste_size);
        return 2;
    }
    if (event->key) {
        char name[32];
        snprintf(name, sizeof name, "%s", SDL_GetKeyName(event->key));
        for (char *letter = name; *letter; letter++)
            *letter = tolower((unsigned char)*letter);
        lua_pushstring(L, name);
        return 2;
    }
    if (*event->text) {
        lua_pushstring(L, event->text);
        return 2;
    }
    return 1;
}

void lxl_damage(SDL_Surface *surface, const SDL_Rect *rects, int count)
{
    SDL_Rect area = {0, 0, surface->w, surface->h};
    for (int i = 0; i < count; i++) {
        SDL_Rect clipped;
        if (SDL_GetRectIntersection(&area, &rects[i], &clipped))
            SDL_GetRectUnion(&damage, &clipped, &damage);
    }
}

void lxl_copy(const char *value)
{
    size_t size = strlen(value);
    if (size <= LX_CLIP_MAX) clip_store(value, size);
    if (size > sizeof clipboard) { copied = 0; return; }
    memcpy(clipboard, value, size);
    copied = size;
}

/* Flat editor regions compress well. Bound the buffer by the raw size and
 * send raw RGB565 whenever the runs would be at least as large. */
static size_t encode_pixels(size_t pixels)
{
    if (pixels < 32)
        return 0;
    size_t bytes = pixels * sizeof *encoded_pixels;
    if (bytes > encoded_capacity) {
        void *storage = realloc(encoded_pixels, bytes);
        if (!storage)
            return 0;
        encoded_pixels = storage;
        encoded_capacity = bytes;
    }
    size_t output = 0;
    for (size_t i = 0; i < pixels;) {
        unsigned short value = packed_pixels[i++];
        unsigned run = 1;
        while (i < pixels && packed_pixels[i] == value && run < 65535) {
            ++i;
            ++run;
        }
        if (output + 2 >= pixels)
            return 0;
        encoded_pixels[output++] = run;
        encoded_pixels[output++] = value;
    }
    return output * sizeof *encoded_pixels;
}

static int present(lua_State *L)
{
    WindowReply reply = {.frame.timer_ms = 50, .clipboard_size = copied};
    SDL_Rect r = full_frame ? (SDL_Rect){0, 0, width, height} : damage;
    SDL_Surface *surface = NULL;
    if (r.w && r.h) {
        surface = SDL_GetWindowSurface(window_renderer.window);
        if (!surface || r.x + r.w > surface->w || r.y + r.h > surface->h)
            return luaL_error(L, "window surface size does not match desktop");
        reply.frame.x = r.x;
        reply.frame.y = r.y;
        reply.frame.w = r.w;
        reply.frame.h = r.h;
    }
    size_t output_size = (size_t)r.w * r.h * sizeof *packed_pixels;
    if (output_size > packed_capacity) {
        void *pixels = realloc(packed_pixels, output_size);
        if (!pixels)
            return luaL_error(L, "out of memory for desktop redraw");
        packed_pixels = pixels;
        packed_capacity = output_size;
    }
    if (surface) {
        char *pixels = (char *)surface->pixels + r.y * surface->pitch +
                       r.x * SDL_BYTESPERPIXEL(surface->format);
        if (!SDL_ConvertPixels(r.w, r.h, surface->format, pixels, surface->pitch,
                               SDL_PIXELFORMAT_RGB565, packed_pixels, r.w * sizeof *packed_pixels))
            return luaL_error(L, "%s", SDL_GetError());
    }
    reply.encoded_size = encode_pixels((size_t)r.w * r.h);
    bool ok = transfer(1, &reply, sizeof reply, true) &&
              (reply.encoded_size ? transfer(1, encoded_pixels, reply.encoded_size, true) :
                                     transfer(1, packed_pixels, output_size, true));
    if (ok && copied)
        ok = transfer(1, clipboard, copied, true);
    copied = 0;
    full_frame = false;
    damage = (SDL_Rect){0};
    lua_pushboolean(L, ok);
    return 1;
}

static int save(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t size;
    const char *text = luaL_checklstring(L, 2, &size);
    const char *slash = strrchr(path, '/');
    int dir_size = slash ? (int)(slash - path + 1) : 0;
    char temporary[513];
    int length = snprintf(temporary, sizeof temporary, "%.*s.lxl.XXXXXX", dir_size, path);
    if (length < 0 || length >= (int)sizeof temporary)
        return luaL_error(L, "%s: directory path is too long", path);

    int fd = mkstemp(temporary);
    if (fd < 0)
        return luaL_error(L, "%s: %s", path, strerror(errno));
    int error = 0;
    if (!transfer(fd, (void *)text, size, true) || fsync(fd) < 0)
        error = errno ? errno : EIO;
    if (close(fd) < 0 && !error)
        error = errno;
    if (!error && rename(temporary, path) < 0)
        error = errno;
    if (error) {
        unlink(temporary);
        return luaL_error(L, "%s: %s", path, strerror(error));
    }
    return 0;
}

void lxl_bind(lua_State *L)
{
    lua_getglobal(L, "system");
    lua_pushcfunction(L, next_event);
    lua_setfield(L, -2, "lixin_next_event");
    lua_pushcfunction(L, present);
    lua_setfield(L, -2, "lixin_present");
    lua_pushcfunction(L, save);
    lua_setfield(L, -2, "lixin_save");
    lua_pop(L, 1);
}

int lxl_shared_paste(lua_State *L)
{
    FILE *file = fopen(LX_CLIP_FILE, "rb");
    if (!file) return 0;
    char *bytes = malloc(LX_CLIP_MAX + 1);
    if (!bytes) { fclose(file); return 0; }
    size_t size = fread(bytes, 1, LX_CLIP_MAX + 1, file);
    int ok = !ferror(file) && size <= LX_CLIP_MAX;
    fclose(file);
    if (ok) lua_pushlstring(L, bytes, size);
    free(bytes);
    return ok;
}
