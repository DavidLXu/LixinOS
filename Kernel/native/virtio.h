#ifndef NATIVE_VIRTIO_H
#define NATIVE_VIRTIO_H
#include "native.h"
enum { Q = 8 };
struct queue {
    struct {
        u64 addr;
        u32 size;
        u16 flags, next;
    } desc[Q] __attribute__((aligned(16)));
    struct {
        u16 flags, index, ring[Q], event;
    } avail;
    volatile struct {
        u16 flags, index;
        struct {
            u32 id, len;
        } ring[Q];
        u16 event;
    } used __attribute__((aligned(4)));
    u16 seen;
};
struct virtio {
    volatile u32 *r;
};
#define BARRIER() asm volatile("dsb sy" ::: "memory")
static int device(struct virtio v, unsigned type, unsigned features) {
    volatile u32 *r = v.r;
    if (r[0] != 0x74726976 || r[1] != 2 || r[2] != type)
        return 0;
    r[0x70 / 4] = 0;
    while (r[0x70 / 4]) {
    }
    r[0x70 / 4] = 3;
    r[0x14 / 4] = 1;
    if (!(r[0x10 / 4] & 1))
        return 0;
    r[0x24 / 4] = 1;
    r[0x20 / 4] = 1;
    r[0x14 / 4] = 0;
    if ((r[0x10 / 4] & features) != features)
        return 0;
    r[0x24 / 4] = 0;
    r[0x20 / 4] = features;
    r[0x70 / 4] = 11;
    return !!(r[0x70 / 4] & 8);
}
static int setup_queue(struct virtio v, unsigned id, struct queue *q) {
    volatile u32 *r = v.r;
    r[0x30 / 4] = id;
    if (r[0x34 / 4] < Q || r[0x44 / 4])
        return 0;
    r[0x38 / 4] = Q;
    r[0x80 / 4] = (u64)q->desc;
    r[0x84 / 4] = 0;
    r[0x90 / 4] = (u64)&q->avail;
    r[0x94 / 4] = 0;
    r[0xa0 / 4] = (u64)&q->used;
    r[0xa4 / 4] = 0;
    q->avail.flags = 1;
    r[0x44 / 4] = 1;
    return 1;
}
static void submit(struct queue *q, unsigned id, void *data, unsigned size, int write) {
    q->desc[id].addr = (u64)data;
    q->desc[id].size = size;
    q->desc[id].flags = write ? 2 : 0;
    q->avail.ring[q->avail.index % Q] = id;
    BARRIER();
    q->avail.index++;
    BARRIER();
}

#endif
