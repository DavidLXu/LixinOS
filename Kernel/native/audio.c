/* Virtio-sound PCM playback on MMIO slot 8. DMA buffers stay in kernel RAM
 * until the device returns their descriptors. PCM is 48 kHz stereo S16LE. */
#include "virtio.h"
extern u64 smp_ticks(void), smp_frequency(void);
enum { INFO = 0x100, PARAMS, PREPARE, RELEASE, START, STOP, OK = 0x8000 };
enum { PERIOD = 4096, PERIODS = Q / 2 };
static struct virtio sound = {(volatile u32 *)0x0a001000};
static struct queue queues[4];
static struct { u32 words[8]; } request;
static u32 reply[9];
static struct { u32 stream; u8 samples[PERIOD]; } data[PERIODS];
static struct { u32 status, latency; } result[PERIODS];
static unsigned busy[PERIODS], stream;
static int initialized, available, running;

static void chain(unsigned queue, unsigned head, void *source, unsigned size,
                  void *dest, unsigned capacity) {
    struct queue *q = &queues[queue];
    q->desc[head].addr = (u64)source;
    q->desc[head].size = size;
    q->desc[head].flags = 1;
    q->desc[head].next = head+1;
    q->desc[head+1].addr = (u64)dest;
    q->desc[head+1].size = capacity;
    q->desc[head+1].flags = 2;
    q->avail.ring[q->avail.index % Q] = head;
    BARRIER();
    ++q->avail.index;
    BARRIER();
    sound.r[0x50/4] = queue;
}
static int control(unsigned cmd, unsigned bytes, unsigned response) {
    struct queue *q = &queues[0];
    request.words[0] = cmd;
    request.words[1] = stream;
    memset(reply, 0, sizeof(reply));
    chain(0, 0, &request, bytes, reply, response);
    u64 start = smp_ticks();
    while (q->used.index == q->seen) {
        if (smp_ticks()-start > smp_frequency()) { available = 0; return 0; }
    }
    BARRIER();
    unsigned slot = q->seen++ % Q;
    return q->used.ring[slot].id == 0 && q->used.ring[slot].len >= response && reply[0] == OK;
}
static int probe(void) {
    if (!device(sound, 25, 0)) return 0;
    for (unsigned q = 0; q < 4; ++q)
        if (!setup_queue(sound, q, &queues[q])) return 0;
    sound.r[0x70/4] = 15;
    unsigned count = sound.r[0x104/4];
    for (stream = 0; stream < count && stream < 32; ++stream) {
        request.words[2] = 1;
        request.words[3] = 32;
        if (!control(INFO, 16, sizeof(reply))) return 0;
        const u8 *caps = (const u8 *)(reply + 1);
        if (!caps[24] && caps[25] <= 2 && caps[26] >= 2 &&
            (reply[3] & (1u << 5)) && (reply[5] & (1u << 7))) return 1;
    }
    return 0;
}
static int reclaim(void) {
    struct queue *q = &queues[2];
    while (q->seen != q->used.index) {
        BARRIER();
        unsigned at = q->seen++ % Q;
        unsigned id = q->used.ring[at].id;
        if (id >= Q || (id & 1) || !busy[id/2] ||
            q->used.ring[at].len != sizeof(result[0]) || result[id/2].status != OK) return 0;
        busy[id/2] = 0;
    }
    return 1;
}
void audio_close(u32 *owned) {
    if (!*owned) return;
    if (available && (!control(STOP, 8, 4) || !control(RELEASE, 8, 4) || !reclaim())) available = 0;
    *owned = running = 0;
}
long audio_call(u32 *owned, u64 bytes, const u8 *mail) {
    if (!initialized) { initialized = 1; available = probe(); }
    if (!available) return -19;
    if (bytes == ~0UL) { audio_close(owned); return 0; }
    if (!bytes) {
        if (*owned) return 0;
        if (running) return -16;
        request.words[2] = PERIOD * PERIODS;
        request.words[3] = PERIOD;
        request.words[4] = 0;
        request.words[5] = 2 | (5 << 8) | (7 << 16);
        if (!control(PARAMS, 24, 4) || !control(PREPARE, 8, 4)) return -5;
        if (!control(START, 8, 4)) { control(RELEASE,8,4); return -5; }
        *owned = running = 1;
        return 0;
    }
    if (!*owned) return -9;
    if (bytes > PERIOD || (bytes & 3)) return -22;
    if (!reclaim()) { available = 0; return -5; }
    unsigned slot = 0;
    while (slot < PERIODS && busy[slot]) ++slot;
    if (slot == PERIODS) return -11;
    busy[slot] = 1;
    data[slot].stream = stream;
    memcpy(data[slot].samples, mail, bytes);
    chain(2, slot*2, &data[slot], bytes+4, &result[slot], sizeof(result[0]));
    return bytes;
}
