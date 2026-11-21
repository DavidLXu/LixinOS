#include "native.h"

/* UART transport used by scripts/ssh-console.py. Each client gets one
 * outstanding data frame until the guest acknowledges its final byte. */
enum { OPEN = 1, DATA = 2, CLOSE = 3, FRAME_LIMIT = 4096, SLOTS = 32 };
struct packet {
    u32 id, size, pos;
    u64 sequence;
    u8 data[FRAME_LIMIT];
};
static struct packet packets[SLOTS];
static u32 active = 1;
static u64 sequence;
static void control(u8 op, u32 id) {
    uart_put(255);
    uart_put(op);
    for (unsigned i = 0; i < 4; i++)
        uart_put(id >> (8 * i));
}
void mux_write(u8 c) {
    uart_put(c);
    if (c == 255)
        uart_put(0);
}
static void pump(void) {
    static u8 header[7];
    static unsigned used, remaining, offset;
    static struct packet *pending;
    for (;;) {
        if (used == 7 && !remaining) {
            if (pending)
                pending->sequence = ++sequence;
            used = 0;
            pending = 0;
            return;
        }
        int c = uart_poll();
        if (c < 0)
            return;
        if (used < 7) {
            header[used++] = c;
            if (used != 7)
                continue;
            u32 id = header[1] | (u32)header[2] << 8 | (u32)header[3] << 16 | (u32)header[4] << 24;
            remaining = header[5] + 256u * header[6];
            offset = 0;
            if (!id || id >= (1u << 30) ||
                !((header[0] == OPEN && !remaining) ||
                  (header[0] == DATA && remaining && remaining <= FRAME_LIMIT)))
                continue;
            for (unsigned i = 0; i < SLOTS; i++)
                if (!packets[i].sequence) {
                    pending = &packets[i];
                    break;
                }
            if (!pending) {
                control(CLOSE, id);
                continue;
            }
            pending->id = id;
            pending->size = remaining;
            pending->pos = 0;
        } else {
            if (pending)
                pending->data[offset++] = c;
            remaining--;
        }
    }
}
static struct packet *first(int current_only) {
    struct packet *next = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct packet *p = &packets[i];
        if (!p->sequence || (current_only && (!p->size || p->id != active)))
            continue;
        if (!next || p->sequence < next->sequence)
            next = p;
    }
    return next;
}
long mux_next(void) {
    struct packet *p;
    while (!(p = first(0))) {
        pump();
        if (!first(0))
            idle_slice();
    }
    if (active != p->id) {
        active = p->id;
        control(OPEN, active);
    }
    if (!p->size) {
        p->sequence = 0;
        return active | (1u << 30);
    }
    return active;
}
int mux_ready(void) {
    pump();
    return first(1) != 0;
}
int mux_key(void) {
    pump();
    struct packet *p = first(1);
    if (!p)
        return -1;
    int c = p->data[p->pos++];
    if (p->pos == p->size) {
        p->sequence = 0;
        control(DATA, active);
    }
    return c;
}
int mux_interrupt(void) {
    pump();
    struct packet *p = first(1);
    if (!p) return 0;
    for (unsigned i = p->pos; i < p->size; i++) {
        if (p->data[i] != 3) continue;
        for (unsigned j = i+1; j < p->size; j++) p->data[j-1] = p->data[j];
        if (--p->size == p->pos) { p->sequence = 0; control(DATA, active); }
        return 1;
    }
    return 0;
}
void mux_close(void) {
    for (unsigned i = 0; i < SLOTS; i++)
        if (packets[i].id == active)
            packets[i].sequence = 0;
    control(CLOSE, active);
}
