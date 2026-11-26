/* Exercise Writer through the same window protocol as Desktop. */
#include <lixin.h>
#include "/src/console.h"
typedef unsigned long size_t;
#define NULL ((void *)0)
#include <stdio.h>
#include <string.h>
#include "/src/gui/text-clipboard.h"
#include "/src/desktop/window-client-host.h"

static WindowClient writer;

static int event(int kind, int x, int y, int value) {
    client_event(&writer, (WindowEvent){kind, x, y, value});
    long deadline = console_call(38, 0) + 180000;
    while (writer.count || writer.busy) {
        if (client_tick(&writer, 0) < 0 || console_call(38, 0) >= deadline)
            return 0;
        if (writer.busy && lx_poll(writer.output, LX_READABLE, 0) <= 0)
            console_call(43, 1);
    }
    return 1;
}

static int settle(void) {
    for (int i = 0; i < 15; ++i) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) return 0;
        console_call(43, 15);
    }
    return 1;
}

static int hold(int ticks) {
    for (int i = 0; i < ticks; ++i) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) return 0;
        console_call(43, 50);
    }
    return 1;
}

static int showcase(void) {
    if (!event(WINDOW_CLIENT_POINTER, 60, 220, 1) ||
        !event(WINDOW_CLIENT_POINTER, 60, 220, 0) || !settle()) return 20;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 6) || !settle()) return 21;
    for (const char *ch = "7319"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 22;
    for (int i = 0; i < 4; ++i)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, 9) || !settle()) return 23;
    for (const char *ch = "7400"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 24;
    if (!settle()) return 25;
    /* Keep the window alive while the host takes a screendump. */
    for (int i = 0; i < 900; ++i) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) return 26;
        console_call(43, 20);
    }
    client_close(&writer);
    return 0;
}

int main(int argc, char **argv) {
    if (!client_open_streams(&writer, "/", "/bin/writer.win3", "/sample.docx", 2))
        return 1;
    if (!event(WINDOW_CLIENT_CONFIGURE, 800, 520, 15) || !settle())
        return 2;
    if (argc > 1 && !strcmp(argv[1], "showcase")) return showcase();
    /* Ctrl+F: find 7319, then replace it with 7400. */
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 6) || !settle()) return 6;
    for (const char *ch = "7319"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 7;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 13) || !settle()) return 8;
    /* Ctrl+H is not expressible over the window protocol (byte 8 is
     * Backspace), so Tab from the find entry to the replace entry. */
    for (int i = 0; i < 4; ++i)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, 9) || !settle()) return 9;
    for (const char *ch = "7400"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 10;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 13) || !settle()) return 11;
    /* Put focus back in the document and select everything. */
    if (!event(WINDOW_CLIENT_POINTER, 120, 300, 1) ||
        !event(WINDOW_CLIENT_POINTER, 120, 300, 0) || !settle()) return 12;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 1) || !settle()) return 13;
    /* Paragraph style first; later direct formatting stays on top. */
    if (!event(WINDOW_CLIENT_POINTER, 145, 53, 1) ||
        !event(WINDOW_CLIENT_POINTER, 145, 53, 0) || !settle()) return 14;
    for (int i = 0; i < 10; ++i)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, 127)) return 15;
    for (const char *ch = "Heading 1"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 16;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 13) || !settle()) return 17;
    /* Size 16: clear the current entry, then type and confirm. */
    if (!event(WINDOW_CLIENT_POINTER, 560, 53, 1) ||
        !event(WINDOW_CLIENT_POINTER, 560, 53, 0) || !settle()) return 18;
    for (int i = 0; i < 10; ++i)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, 127)) return 19;
    for (const char *ch = "16"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 20;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 13) || !settle()) return 21;
    /* Font family. */
    if (!event(WINDOW_CLIENT_POINTER, 345, 53, 1) ||
        !event(WINDOW_CLIENT_POINTER, 345, 53, 0) || !settle()) return 22;
    for (int i = 0; i < 40; ++i)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, 127)) return 23;
    for (const char *ch = "Fira Sans"; *ch; ++ch)
        if (!event(WINDOW_CLIENT_KEY, 0, 0, *ch)) return 24;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 13) || !settle()) return 25;
    /* Underline, then centre the paragraphs. */
    if (!event(WINDOW_CLIENT_POINTER, 129, 89, 1) ||
        !event(WINDOW_CLIENT_POINTER, 129, 89, 0) || !settle()) return 26;
    if (!event(WINDOW_CLIENT_POINTER, 233, 89, 1) ||
        !event(WINDOW_CLIENT_POINTER, 233, 89, 0) || !settle()) return 27;
    /* Let the queued UNO formatting commands run before saving. */
    if (!hold(20)) return 28;
    /* Save the edited DOCX. */
    if (!event(WINDOW_CLIENT_POINTER, 120, 300, 1) ||
        !event(WINDOW_CLIENT_POINTER, 120, 300, 0) || !settle()) return 28;
    if (!event(WINDOW_CLIENT_KEY, 0, 0, 19) || !settle()) return 29;
    client_close(&writer);
    return 0;
}
