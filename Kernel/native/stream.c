#include "native.h"

/* One bounded ring per connection. Each live endpoint owns a reference. */
#define PIPE_BYTES 8192
struct pipe {
    unsigned readers, writers, read, write;
    u8 data[PIPE_BYTES];
};
static struct pipe pipes[NATIVE_TASKS];
unsigned pipe_ready(struct pipe *p, int write) {
    unsigned used = p->write - p->read;
    if (write)
        return used < PIPE_BYTES || !p->readers ? 2 : 0;
    return used || !p->writers;
}
struct pipe *pipe_create(void) {
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
        struct pipe *p = &pipes[i];
        if (p->readers || p->writers)
            continue;
        p->readers = p->writers = 1;
        p->read = p->write = 0;
        return p;
    }
    return 0;
}
void pipe_ref(struct pipe *p, int writer, int delta) {
    if (p) {
        if (writer)
            p->writers += delta;
        else
            p->readers += delta;
    }
}
long pipe_read(struct pipe *p, u8 *dst, unsigned size) {
    if (!size)
        return 0;
    while (p->read == p->write) {
        if (!p->writers || task_stopped())
            return 0;
        schedule();
    }
    unsigned n = p->write - p->read;
    if (n > size)
        n = size;
    for (unsigned i = 0; i < n; i++)
        dst[i] = p->data[p->read++ % PIPE_BYTES];
    return n;
}
long pipe_write(struct pipe *p, const u8 *src, unsigned size) {
    unsigned done = 0;
    while (done < size) {
        if (!p->readers || task_stopped())
            return done ? (long)done : -1;
        unsigned room = PIPE_BYTES - (p->write - p->read);
        if (!room) {
            schedule();
            continue;
        }
        if (room > size - done)
            room = size - done;
        for (unsigned i = 0; i < room; i++)
            p->data[p->write++ % PIPE_BYTES] = src[done++];
    }
    return done;
}
