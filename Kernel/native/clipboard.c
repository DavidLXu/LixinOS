#include "virtio.h"

static struct virtio port = {(volatile u32 *)0x0a000c00};
static struct queue rx, tx;
static u8 buffers[Q][256], output[8197];
static u8 document[1 << 20], paste[4096];
static unsigned length, online, seq, waiting, paste_at, paste_len, multi;
static u64 destination, deadline;
void clipboard_init(void) {
    if (!device(port, 3, 0) || !setup_queue(port, 0, &rx) || !setup_queue(port, 1, &tx))
        return;
    for (unsigned i = 0; i < Q; i++)
        submit(&rx, i, buffers[i], sizeof(buffers[i]), 1);
    port.r[0x70 / 4] = 15;
    port.r[0x50 / 4] = 0;
    online = 1;
}
static void send(unsigned size) {
    if (!online)
        return;
    submit(&tx, 0, output, size, 0);
    port.r[0x50 / 4] = 1;
    u64 start = clock_ticks();
    while (tx.seen == tx.used.index)
        if (clock_ticks() - start > clock_hz()) {
            online = 0;
            return;
        }
    BARRIER();
    tx.seen++;
}
void clipboard_copy(const u8 *src, unsigned size) {
    if (!size || size > sizeof(document))
        return;
    memcpy(document, src, size);
    length = size;
    if (!online || size > sizeof(paste))
        return;
    const char *hex = "0123456789abcdef";
    output[0] = 'C';
    output[1] = '\t';
    for (unsigned i = 0; i < size; i++) {
        output[2 + 2 * i] = hex[src[i] >> 4];
        output[3 + 2 * i] = hex[src[i] & 15];
    }
    output[2 + 2 * size] = '\n';
    send(3 + 2 * size);
}
unsigned clipboard_document(u8 *out) {
    memcpy(out, document, length);
    return length;
}
static void prepare(const u8 *bytes, unsigned n) {
    paste_len = paste_at = 0;
    waiting = 0;
    for (unsigned i = 0; i < n && paste_len < sizeof(paste); i++) {
        unsigned c = bytes[i];
        if (c == '\n' && i && bytes[i - 1] == '\r')
            continue;
        if (c == '\r' || c == '\n')
            c = multi ? '\r' : ' ';
        if (c == '\t')
            c = multi ? '\t' : ' ';
        if ((c >= 32 && c <= 126) || c == '\r' || c == '\t')
            paste[paste_len++] = c;
    }
}
void clipboard_request(int multiline) {
    multi = multiline;
    destination = task_pid();
    paste_len = paste_at = 0;
    if (!online) {
        prepare(document, length);
        return;
    }
    waiting = ++seq;
    if (!waiting)
        waiting = ++seq;
    deadline = clock_ticks() + 3 * clock_hz();
    char *end = add_number(add_text((char *)output, "P\t"), waiting);
    *end++ = '\n';
    send(end - (char *)output);
}
int clipboard_key(void) {
    if (destination != task_pid())
        return -1;
    if (waiting && clock_ticks() > deadline)
        waiting = 0;
    return paste_at < paste_len ? paste[paste_at++] : -1;
}
static unsigned number(const char **s) {
    unsigned n = 0;
    while (**s >= '0' && **s <= '9') {
        if (n > 429496728)
            return 0;
        n = n * 10 + *(*s)++ - '0';
    }
    return n;
}
static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? 10 + c - 'a' : -1;
}
static void line(const char *s) {
    if (s[0] == 'R' && s[1] == '\t') {
        s += 2;
        unsigned w = number(&s);
        if (*s++ != '\t')
            return;
        unsigned h = number(&s);
        if (!*s)
            resize_display(w, h);
        return;
    }
    unsigned id = number(&s);
    if (!waiting || id != waiting || *s++ != '\t')
        return;
    static u8 bytes[4096];
    unsigned n = 0;
    while (*s) {
        int a = hex(*s++);
        if (a < 0 || !*s)
            return;
        int b = hex(*s++);
        if (b < 0 || n == sizeof(bytes))
            return;
        bytes[n++] = 16 * a + b;
    }
    prepare(bytes, n);
}
void clipboard_poll(void) {
    static char packet[8224];
    static unsigned used, overflow;
    if (!online)
        return;
    while (rx.seen != rx.used.index) {
        BARRIER();
        unsigned at = rx.seen++ % Q, id = rx.used.ring[at].id, n = rx.used.ring[at].len;
        if (id >= Q || n > sizeof(buffers[0])) {
            online = 0;
            return;
        }
        for (unsigned i = 0; i < n; i++) {
            char c = buffers[id][i];
            if (c == '\n') {
                packet[used] = 0;
                if (!overflow)
                    line(packet);
                used = overflow = 0;
            } else if (used < sizeof(packet) - 1)
                packet[used++] = c;
            else
                overflow = 1;
        }
        submit(&rx, id, buffers[id], sizeof(buffers[id]), 1);
        port.r[0x50 / 4] = 0;
    }
}
