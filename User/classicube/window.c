/* ClassiCube's window backend for the foreground LixinOS canvas. */
#include "_WindowBase.h"
#include "Bitmap.h"
#include "Errors.h"
#include "Entity.h"
#include "World.h"
#include "../console.h"
#include "../gui/lixin-gui.h"
#include <stdio.h>

#include <string.h>

static struct lx_canvas canvas;
static int mx, my, inside, closing, right_click, tracing;
static float key_time[INPUT_COUNT];
int lixin_flat_world;
static int last_x, last_z, reported;

int lixin_game_open(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--trace")) tracing = 1;
        else if (!strcmp(argv[i], "--flat")) lixin_flat_world = 1;
        else {
            puts("Usage: classicube [--flat] [--trace]");
            return 0;
        }
    }
    if (lx_gui_open(&canvas)) {
        puts("ClassiCube requires the graphical LixinOS shell (--window).");
        return 0;
    }
    return 1;
}

static void block_changed(void *obj, IVec3 pos, BlockID old, BlockID block) {
    printf("Block %d %d %d: %u -> %u\n", pos.x, pos.y, pos.z, old, block);
    fflush(stdout);
}

void Window_PreInit(void) { DisplayInfo.CursorVisible = true; }
void Window_Init(void) {
    Input.Sources = INPUT_SOURCE_NORMAL;
    DisplayInfo.Width = Window_Main.Width = canvas.width;
    DisplayInfo.Height = Window_Main.Height = canvas.height;
    DisplayInfo.Depth = 32;
    DisplayInfo.ScaleX = DisplayInfo.ScaleY = 1;
    Platform_Flags |= PLAT_FLAG_SINGLE_PROCESS;
    if (tracing) Event_Register_(&UserEvents.BlockChanged, NULL, block_changed);
}
void Window_Free(void) {}
void Window_Create3D(int w, int h) {
    Window_Main.Exists = Window_Main.Focused = true;
    Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
    Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;
}
void Window_Create2D(int w, int h) { Window_Create3D(w, h); }
void Window_Destroy(void) {}
void Window_SetTitle(const cc_string *title) {}
void Clipboard_GetText(cc_string *value) {}
void Clipboard_SetText(const cc_string *value) {}
int Window_GetWindowState(void) { return WINDOW_STATE_FULLSCREEN; }
cc_result Window_EnterFullscreen(void) { return 0; }
cc_result Window_ExitFullscreen(void) { return 0; }
int Window_IsObscured(void) { return 0; }
void Window_Show(void) {}
void Window_SetSize(int w, int h) {}
void Window_RequestClose(void) { closing = 1; }

static void pulse(int key) {
    Input_SetNonRepeatable(key, true);
    key_time[key] = 0.14f;
}

static int escape_key(int prefix) {
    int ch = lx_timed_byte(0);
    if (ch >= 'A' && ch <= 'D') {
        static const int arrows[] = { CCKEY_UP, CCKEY_DOWN, CCKEY_RIGHT, CCKEY_LEFT };
        return arrows[ch - 'A'];
    }
    if (prefix == 'O' && ch >= 'P' && ch <= 'S') return CCKEY_F1 + ch - 'P';
    if (ch == 'H') return CCKEY_HOME;
    if (ch == 'F') return CCKEY_END;
    if (ch < '0' || ch > '9') return 0;
    int code = ch - '0';
    ch = lx_timed_byte(0);
    if (ch >= '0' && ch <= '9') {
        code = 10 * code + ch - '0';
        ch = lx_timed_byte(0);
    }
    if (ch != '~') return 0;
    switch (code) {
    case 2: return CCKEY_INSERT;
    case 3: return CCKEY_DELETE;
    case 5: return CCKEY_PAGEUP;
    case 6: return CCKEY_PAGEDOWN;
    case 15: return CCKEY_F5;
    case 17: case 18: case 19: case 20: case 21: return CCKEY_F6 + code - 17;
    case 23: case 24: return CCKEY_F11 + code - 23;
    default: return 0;
    }
}

static void keyboard(int byte) {
    int key = 0;
    if (byte == 17) { Window_RequestClose(); return; }
    if (byte >= 'a' && byte <= 'z') key = byte - 'a' + CCKEY_A;
    else if ((byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) key = byte;
    else if (byte == 27) {
        int next = lx_timed_byte(0);
        if (next == '[' || next == 'O') {
            key = escape_key(next);
            if (key) pulse(key);
            return;
        }
        pulse(CCKEY_ESCAPE);
        if (next >= 0 && next < 256) keyboard(next);
        return;
    } else if (byte == ' ') key = CCKEY_SPACE;
    else if (byte == '\r' || byte == '\n') key = CCKEY_ENTER;
    else if (byte == 127 || byte == 8) key = CCKEY_BACKSPACE;
    else if (byte == '\t') key = CCKEY_TAB;
    else if (byte == '/') key = CCKEY_SLASH;
    if (key) pulse(key);
    if (byte >= 32 && byte < 127) Event_RaiseInt(&InputEvents.Press, byte);
}

void Window_ProcessEvents(float delta) {
    for (int k = 0; k < INPUT_COUNT; ++k) {
        if (key_time[k] > 0 && (key_time[k] -= delta) <= 0)
            Input_SetNonRepeatable(k, false);
    }
    for (int i = 0; i < 64; ++i) {
        int c = lx_timed_byte(0);
        if (c < 0 || c > 255) break;
        keyboard(c);
    }
    if (right_click) { Input_SetNonRepeatable(CCMOUSE_R, false); right_click = 0; }
    for (int i = 0; i < 64; ++i) {
        long ev = console_call(21, 8);
        struct lx_pointer p;
        if (!lx_gui_decode_pointer(&p, ev)) break;
        if (p.cancelled || !p.inside) {
            inside = 0;
            Input_SetNonRepeatable(CCMOUSE_L, false);
            continue;
        }
        if (inside && Input.RawMode)
            Event_RaiseRawMove(&PointerEvents.RawMoved, (int)p.x - mx, (int)p.y - my);
        inside = 1; mx = p.x; my = p.y;
        Pointer_SetPosition(0, mx, my);
        Input_SetNonRepeatable(CCMOUSE_L, p.down);
        if (ev & (1L << 29)) { Input_SetNonRepeatable(CCMOUSE_R, true); right_click = 1; }
        if (p.wheel) Mouse_ScrollVWheel(p.wheel);
    }
    if (Input.RawMode) {
        int dx = Input.Pressed[CCKEY_RIGHT] - Input.Pressed[CCKEY_LEFT];
        int dy = Input.Pressed[CCKEY_DOWN] - Input.Pressed[CCKEY_UP];
        /* The virtio tablet has absolute coordinates and cannot be warped. */
        if (inside && mx < 4) dx--;
        if (inside && mx >= (int)canvas.width - 4) dx++;
        if (dx || dy) Event_RaiseRawMove(&PointerEvents.RawMoved, dx*delta*500, dy*delta*500);
    }
    if (tracing && World.Loaded && Entities.CurPlayer) {
        Vec3 p = Entities.CurPlayer->Base.Position;
        if (!reported || (int)p.x != last_x || (int)p.z != last_z) {
            printf("Player %d %d %d\n", (int)p.x, (int)p.y, (int)p.z);
            fflush(stdout);
            reported = 1; last_x = (int)p.x; last_z = (int)p.z;
        }
    }
    if (closing) {
        Window_Main.Exists = false;
        Event_RaiseVoid(&WindowEvents.Closing);
    }
}
void Gamepads_PreInit(void) {}
void Gamepads_Init(void) {}
void Gamepads_Process(float delta) {}
static void Cursor_GetRawPos(int *x, int *y) { *x = mx; *y = my; }
void Cursor_SetPosition(int x, int y) {}
static void Cursor_DoSetVisible(cc_bool visible) {}
static void ShowDialogCore(const char *title, const char *msg) { fprintf(stderr, "%s: %s\n", title, msg); }
cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs *args) { return ERR_NOT_SUPPORTED; }
cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs *args) { return ERR_NOT_SUPPORTED; }
void Window_AllocFramebuffer(struct Bitmap *bmp, int w, int h) {
    bmp->width = w; bmp->height = h;
    bmp->scan0 = Mem_Alloc(w*h, sizeof(BitmapCol), "window pixels");
}
void Window_FreeFramebuffer(struct Bitmap *bmp) { Mem_Free(bmp->scan0); }
void Window_DrawFramebuffer(Rect2D r, struct Bitmap *bmp) {
    if (bmp->width != canvas.width || bmp->height != canvas.height) return;
    memcpy(canvas.pixels, bmp->scan0, canvas.width * canvas.height * sizeof(BitmapCol));
    if (!Input.RawMode && inside) {
        lx_gui_rect(&canvas, mx, my, 3, 11, 0xffffff);
        lx_gui_rect(&canvas, mx, my, 8, 3, 0xffffff);
    }
    lx_gui_present(&canvas);
}
void OnscreenKeyboard_Open(struct OpenKeyboardArgs *args) {}
void OnscreenKeyboard_SetText(const cc_string *text) {}
void OnscreenKeyboard_Close(void) {}
void Window_EnableRawMouse(void) { DefaultEnableRawMouse(); }
void Window_UpdateRawMouse(void) {}
void Window_DisableRawMouse(void) { DefaultDisableRawMouse(); }
