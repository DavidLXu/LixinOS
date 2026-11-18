#include "runtime.h"
#include "../console.h"
#include <stdio.h>
#include <string.h>
void htop(void) {
    const int window = screen_info() >> 32;
    const int width = (screen_info() & 65535) < 60 ? 8 : 32;
    if (!window)
        screen_write("\033[?1049h\033[2J", 12);
    for (;;) {
        if (console_call(16, 0))
            return;
        unsigned long s[8];
        memcpy(s, (void *)0x443d8000, sizeof(s));
        char bar[33], frame[640];
        int filled = width * s[1] / (s[1] + s[2]);
        memset(bar, '#', filled);
        memset(bar + filled, '-', width - filled);
        bar[width] = 0;
        int n = snprintf(frame, sizeof(frame),
                         "%sLixinOS   htop                         q: quit\r\n\r\n"
                         "Uptime  %lu s          CPUs  1\033[K\r\nGuest RAM  %lu MiB    Kernel "
                         "stack  %lu KiB\r\n"
                         "Heap [%s] %lu B used / %lu B free\033[K\r\nFilesystem  %lu files, %lu "
                         "dirs, %lu bytes\033[K\r\n"
                         "\r\nCONTEXT       STATE       ACTIVITY\r\nUser program  foreground  htop "
                         "(console polling)\r\n",
                         window ? "" : "\033[H", s[0], s[7] / (1024 * 1024), s[6], bar, s[1], s[2],
                         s[3], s[4], s[5]);
        screen_write(frame, n);
        int c = read_timed(1000);
        if (c == 'q' || c == 'Q' || c == 27)
            break;
        if (window)
            screen_write("\033[9A", 4);
    }
    if (!window)
        screen_write("\033[?1049l", 8);
}
