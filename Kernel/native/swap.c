/* Disposable block paging store on a dedicated virtio-mmio device. Only a
 * volume with our header can accept writes. Sector zero holds the header. */
#include "virtio.h"
#define BLOCK_BYTES (2UL << 20)
#define MAX_SLOTS 8192
static struct virtio drive = {(volatile u32 *)0x0a001200};
static struct queue queue;
static struct { u32 op, reserved; u64 sector; } request;
static volatile u8 status;
static u8 header[512] __attribute__((aligned(16)));
static u8 slots[MAX_SLOTS];
static unsigned total, allocated, hint, resident_limit;
static int probed, failed;
extern u64 smp_ticks(void), smp_frequency(void);

static int transfer(int write, u64 sector, void *data, u32 size) {
    if (failed) return 0;
    request.op = write ? 1 : 0;
    request.sector = sector;
    status = 255;
    queue.desc[0].addr = (u64)&request;
    queue.desc[0].size = 16; queue.desc[0].flags = 1; queue.desc[0].next = 1;
    queue.desc[1].addr = (u64)data;
    queue.desc[1].size = size; queue.desc[1].flags = write ? 1 : 3; queue.desc[1].next = 2;
    queue.desc[2].addr = (u64)&status;
    queue.desc[2].size = 1; queue.desc[2].flags = 2;
    u16 seq = queue.avail.index;
    queue.avail.ring[seq % Q] = 0;
    BARRIER(); queue.avail.index = seq+1; BARRIER();
    drive.r[0x50/4] = 0;
    u64 start = smp_ticks(), timeout = smp_frequency()*5;
    while (queue.used.index == seq) {
        if (smp_ticks()-start > timeout) {
            drive.r[0x70/4] = 0;
            while (drive.r[0x70/4]) {}
            failed = 1;
            return 0;
        }
    }
    BARRIER();
    drive.r[0x64/4] = drive.r[0x60/4];
    return queue.used.ring[seq % Q].id == 0 && !status;
}
unsigned vm_swap_total(void) {
    if (probed) return total;
    probed = 1;
    if (!device(drive, 2, 0) || (drive.r[0x10/4] & (1u << 5)) || !setup_queue(drive, 0, &queue)) return 0;
    drive.r[0x70/4] = 15;
    u64 sectors = (u64)drive.r[0x104/4]<<32 | drive.r[0x100/4];
    if (sectors < 4097 || !transfer(0, 0, header, sizeof(header)) || memcmp(header, "LXSWAP01", 8)) return 0;
    u64 capacity = (sectors-1)/4096;
    total = capacity > MAX_SLOTS ? MAX_SLOTS : capacity;
    resident_limit = (unsigned)header[8] | ((unsigned)header[9] << 8);
    return total;
}
unsigned vm_swap_free(void) { return vm_swap_total()-allocated; }
unsigned vm_swap_limit(void) { return resident_limit; }
unsigned vm_swap_save(void *frame) {
    if (allocated >= total || failed) return 0;
    for (unsigned i=0; i<total; ++i) {
        if (hint >= total) hint = 0;
        unsigned slot = hint++;
        if (slots[slot]) continue;
        if (!transfer(1, 1+(u64)slot*4096, frame, BLOCK_BYTES)) return 0;
        slots[slot] = 1;
        ++allocated;
        return slot+1;
    }
    return 0;
}
int vm_swap_load(unsigned id, void *frame) {
    return id && id <= total && slots[id-1] && transfer(0, 1+(u64)(id-1)*4096, frame, BLOCK_BYTES);
}
void vm_swap_discard(unsigned id) {
    if (id && id <= total && slots[id-1]) { slots[id-1]=0; --allocated; }
}
