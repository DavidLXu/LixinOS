#include "../console.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
extern char __user_heap_start[], __user_heap_limit[];
#define ARENA 0x68000000UL
#define FILE_LIMIT (1024UL * 1024)
static int editing;
void *_sbrk(ptrdiff_t size) {
    static char *end;
    if (!end)
        end = editing ? (char *)(ARENA + FILE_LIMIT + 16) : __user_heap_start;
    char *limit = editing ? (char *)(ARENA + 128UL * 1024 * 1024) : __user_heap_limit;
    if (size < 0 || size > limit - end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    if (editing && console_call(17, (unsigned long)(end + size) - ARENA)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *old = end;
    end += size;
    return old;
}
static char *const mailbox = (char *)ARENA;
unsigned long screen_info(void) { return console_call(13, 0); }
int read_timed(unsigned milliseconds) { return console_call(14, milliseconds); }
int read_raw(void) { return get_byte(); }
void screen_write(const char *bytes, size_t len) {
    for (size_t i = 0; i < len; i++)
        put_byte(bytes[i]);
}
int save_file(const char *bytes, size_t len) {
    if (len > FILE_LIMIT)
        return 1;
    memcpy(mailbox, bytes, len);
    return console_call(15, len);
}
void _exit(int status) {
    const char *end =
        screen_info() >> 32 ? "\033[0m\033[2J\033[H" : "\033[0m\033[2J\033[H\033[?25h\033[?1049l";
    screen_write(end, strlen(end));
    finish(status);
}
__attribute__((section(".text.start"), noreturn)) void _start(const char *mode, const char *source,
                                                              size_t len) {
    if (!strcmp(mode, "htop")) {
        htop();
        finish(0);
    }
    editing = 1;
    len = strtoul(source, NULL, 10);
    char *name = strdup(mode + strlen(mode) + 1);
    char *text = malloc(len + 1);
    if (!name || !text)
        finish(1);
    memcpy(text, mailbox, len);
    text[len] = 0;
    if (!strcmp(mode,"paint")) { extern int paint(const char *,const char *,size_t); finish(paint(name,text,len)); }
    if (!strcmp(mode, "nano"))
        nano(name, text, len);
    else
        kilo(name, text, len);
    finish(0);
}
