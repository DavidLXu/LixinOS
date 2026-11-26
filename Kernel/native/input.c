#include "native.h"

/* Virtio input event queue on QEMU virt. There is no status-queue traffic. */
enum { QSIZE = 32 };
struct event {
    u16 type, code;
    u32 value;
};
struct input_queue {
    volatile u32 *regs;
    struct {
        u64 addr;
        u32 len;
        u16 flags, next;
    } desc[QSIZE] __attribute__((aligned(16)));
    struct {
        u16 flags, index, ring[QSIZE], event;
    } avail;
    volatile struct {
        u16 flags, index;
        struct {
            u32 id, len;
        } ring[QSIZE];
        u16 event;
    } used __attribute__((aligned(4)));
    struct event events[QSIZE];
    u16 last;
};
static struct input_queue keyboard, tablet;
#define BARRIER() asm volatile("dsb sy" ::: "memory")

static void set_address(volatile u32 *regs, unsigned offset, u64 address) {
    regs[offset / 4] = address;
    regs[offset / 4 + 1] = address >> 32;
}
static void setup(struct input_queue *q, u64 address) {
    volatile u32 *r = (volatile u32 *)address;
    if (r[0] != 0x74726976 || r[1] != 2 || r[2] != 18)
        return;
    r[0x70 / 4] = 0;
    while (r[0x70 / 4]) {
    }
    r[0x70 / 4] = 3;
    r[0x14 / 4] = 1;
    if (!(r[0x10 / 4] & 1))
        return;
    r[0x24 / 4] = 1;
    r[0x20 / 4] = 1;
    r[0x24 / 4] = 0;
    r[0x20 / 4] = 0;
    r[0x70 / 4] = 11;
    if (!(r[0x70 / 4] & 8))
        return;
    r[0x30 / 4] = 0;
    if (r[0x34 / 4] < QSIZE || r[0x44 / 4])
        return;
    r[0x38 / 4] = QSIZE;
    set_address(r, 0x80, (u64)q->desc);
    set_address(r, 0x90, (u64)&q->avail);
    set_address(r, 0xa0, (u64)&q->used);
    for (unsigned i = 0; i < QSIZE; i++) {
        q->desc[i].addr = (u64)&q->events[i];
        q->desc[i].len = sizeof(struct event);
        q->desc[i].flags = 2;
        q->avail.ring[i] = i;
    }
    q->avail.index = QSIZE;
    BARRIER();
    r[0x44 / 4] = 1;
    r[0x70 / 4] = 15;
    r[0x50 / 4] = 0;
    q->regs = r;
}

void input_init(void) {
    setup(&keyboard, 0x0a000200);
    setup(&tablet, 0x0a000600);
}

static int event(struct input_queue *q, struct event *out) {
    if (!q->regs || q->used.index == q->last)
        return 0;
    BARRIER();
    u16 at = q->last++ % QSIZE;
    u32 id = q->used.ring[at].id;
    if (id >= QSIZE || q->used.ring[at].len != sizeof(*out)) {
        q->regs = 0;
        return 0;
    }
    *out = q->events[id];
    q->avail.ring[q->avail.index % QSIZE] = id;
    BARRIER();
    q->avail.index++;
    BARRIER();
    q->regs[0x50 / 4] = 0;
    q->regs[0x64 / 4] = q->regs[0x60 / 4];
    return 1;
}

static unsigned modifiers, navigation_keys;
/* Canvas geometry request 2 reports held Up, Down, Left, Right and Space. */
unsigned canvas_navigation_keys(void) { return navigation_keys; }
static unsigned navigation_mask(unsigned code) {
    switch (code) {
    case 103: return 1;
    case 108: return 2;
    case 105: return 4;
    case 106: return 8;
    case 57: return 16;
    default: return 0;
    }
}
int keyboard_poll(void) {
    static unsigned caps;
    static const char *pending = "";
    static const char normal[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";
    static const char shifted[] = "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
    static const char *nav[] = {"\033[H", "\033[A", "\033[5~", "\033[D",  "\033[C",
                                "\033[F", "\033[B", "\033[6~", "\033[2~", "\033[3~"};
    static const char *app_nav[] = {"\033OH","\033OA",0,"\033OD","\033OC","\033OF","\033OB",0,0,0};
    static const char *fn[] = {"\033OP",   "\033OQ",   "\033OR",   "\033OS",
                               "\033[15~", "\033[17~", "\033[18~", "\033[19~",
                               "\033[20~", "\033[21~", "\033[23~", "\033[24~"};
    struct event e;
    if (*pending)
        return *pending++;
    while (event(&keyboard, &e)) {
        if (e.type != 1)
            continue;
        u32 code = e.code, mask = code == 42   ? 1
                                  : code == 54 ? 2
                                  : code == 29 ? 4
                                  : code == 97 ? 8
                                  : code == 56 ? 16
                                  : code == 100 ? 32
                                               : 0;
        unsigned held = navigation_mask(code);
        if (e.value) navigation_keys |= held; else navigation_keys &= ~held;
        if (mask) {
            if (e.value)
                modifiers |= mask;
            else
                modifiers &= ~mask;
            continue;
        }
        if (!e.value)
            continue;
        if (code == 58) {
            if (e.value == 1)
                caps ^= 1;
            continue;
        }
        if ((modifiers & 3) && (modifiers & 12) && (code == 46 || code == 47))
            return code == 46 ? 258 : 259;
        if (code == 1)
            return 27;
        if (code == 14)
            return 127;
        if (code == 15)
            return modifiers & 12 ? 257 : 9;
        if (code == 28 || code == 96)
            return 13;
        if (code == 57)
            return 32;
        if (code >= 102 && code <= 111) {
            unsigned k = code - 102;
            pending = nav[k];
            if (display_application_cursor() && app_nav[k])
                pending = app_nav[k];
        }
        else if (code >= 59 && code <= 68)
            pending = fn[code - 59];
        else if (code == 87 || code == 88)
            pending = fn[code - 77];
        if (*pending)
            return *pending++;
        if (code < 2 || code - 2 >= sizeof(normal) - 1)
            continue;
        int ch = normal[code - 2];
        if (!ch)
            continue;
        if ((!!(modifiers & 3)) ^ (caps && ch >= 'a' && ch <= 'z'))
            ch = shifted[code - 2];
        return modifiers & 12 ? ch & 31 : ch;
    }
    return -1;
}

static unsigned pointer_down, suppress_pointer, pointer_x, pointer_y;
static int pointer_seen, right_down, right_press, middle_down;
unsigned input_modifiers(void) {
    return !!(modifiers & 3) | (!!(modifiers & 12) << 1) | (!!(modifiers & 48) << 2);
}
long tablet_position(unsigned width, unsigned height, unsigned bar) {
    if (!pointer_seen)
        return -1;
    unsigned x = pointer_x * (width - 1) / 32767, y = pointer_y * (height - 1) / 32767;
    return x / 2 | ((y >= bar ? (y - bar) / 2 : 0) << 12) | (y >= bar) << 25;
}
long tablet_poll(unsigned width, unsigned height, unsigned bar) {
    static unsigned x, y;
    static int wheel;
    struct event e;
    while (event(&tablet, &e)) {
        if (e.type == 3 && e.code == 0)
            x = e.value > 32767 ? 32767 : e.value;
        if (e.type == 3 && e.code == 1)
            y = e.value > 32767 ? 32767 : e.value;
        if (e.type == 1 && e.code == 272) {
            pointer_down = !!e.value;
            if (!pointer_down) suppress_pointer = 0;
        }
        if (e.type == 1 && e.code == 273) {
            right_press |= e.value && !right_down;
            right_down = !!e.value;
        }
        if (e.type == 1 && e.code == 274) middle_down = !!e.value;
        if (e.type == 2 && e.code == 8)
            wheel = (int)e.value;
        if (e.type == 0 && e.code == 0) {
            pointer_seen = 1;
            pointer_x = x;
            pointer_y = y;
            long val = tablet_position(width, height, bar) | (pointer_down && !suppress_pointer) << 24 |
                       (wheel > 0) << 26 | (wheel < 0) << 27 | (long)right_press << 29 |
                       (long)input_modifiers() << 32 | (long)right_down << 35 | (long)middle_down << 36;
            right_press = 0;
            wheel = 0;
            return val;
        }
    }
    return -1;
}

void suppress_tablet_left(void) { suppress_pointer = pointer_down; }

void reset_tablet(unsigned width, unsigned height, unsigned bar) {
    while (tablet_poll(width, height, bar) >= 0) {}
    suppress_pointer = pointer_down;
}
