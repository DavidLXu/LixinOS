#pragma once

/* One request in flight per client. stdin receives an event; stdout returns
 * a full RGB565 surface. The desktop owns window chrome and pointer capture. */
enum { WINDOW_CLIENT_WIDTH = 128, WINDOW_CLIENT_HEIGHT = 72 };
enum { WINDOW_CLIENT_POINTER, WINDOW_CLIENT_KEY };
typedef struct {
    short type, x, y, value;
} WindowEvent;
typedef unsigned short WindowFrame[WINDOW_CLIENT_HEIGHT][WINDOW_CLIENT_WIDTH];
typedef char WindowEventSizeCheck[sizeof(WindowEvent) == 8 && sizeof(unsigned short) == 2 ? 1 : -1];

/* .win2 uses the same input record. CONFIGURE gives the content width/height.
 * Every event receives one WindowUpdate and then w*h packed RGB565 pixels. */
enum { WINDOW_CLIENT_CONFIGURE = 2, WINDOW_CLIENT_TIMER = 3 };
typedef struct {
    unsigned short x, y, w, h, timer_ms;
} WindowUpdate;
typedef char WindowUpdateSizeCheck[sizeof(WindowUpdate) == 10 ? 1 : -1];

/* .win3 adds wheel and close requests. After the rectangle, the client may
 * return up to 4096 UTF-8 bytes to copy to the system clipboard. */
enum { WINDOW_CLIENT_WHEEL = 4, WINDOW_CLIENT_CLOSE = 5, WINDOW_CLIENT_PASTE = 6,
       WINDOW_CLIENT_RIGHT_CLICK = 7,
       WINDOW_CLIENT_CLIPBOARD_MAX = 4096 };
/* CONFIGURE.value advertises optional input. A reply can opt in using the
 * high bit of frame.timer_ms. Older hosts advertise zero. */
enum { WINDOW_CLIENT_RIGHT_CLICK_AVAILABLE = 1, WINDOW_CLIENT_WANT_RIGHT_CLICK = 0x8000 };
typedef struct {
    WindowUpdate frame;
    unsigned short clipboard_size;
    unsigned encoded_size;
} WindowReply;
typedef char WindowReplySizeCheck[sizeof(WindowReply) == 16 ? 1 : -1];
