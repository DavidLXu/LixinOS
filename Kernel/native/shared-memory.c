#include "smp.h"
#include "native.h"

#define PAGE 4096U
#define TABLE_SPAN (512 * PAGE)
#define MAP_BLOCKS (LX_SHARED_APERTURE / TABLE_SPAN)
#define SHARED_FILES LX_DESCRIPTOR_LIMIT

struct shared_file {
    unsigned refs, mappings, size, position;
    u8 *data;
};
static struct shared_file files[SHARED_FILES];

struct mapping {
    struct shared_file *file;
    unsigned bytes, count;
    u64 (*tables)[512];
};
static struct mapping maps[NATIVE_TASKS][MAP_BLOCKS];
static unsigned map_end[NATIVE_TASKS];
static u64 aperture[2][512] __attribute__((aligned(PAGE)));
static unsigned mapped_banks[2] = {NATIVE_TASKS,NATIVE_TASKS};
#define mapped_bank mapped_banks[smp_cpu()]

extern void sync_image(void);

struct shared_file *shared_create(void) {
    for (unsigned i = 0; i < SHARED_FILES; i++) {
        if (files[i].refs) continue;
        memset(&files[i], 0, sizeof(files[i]));
        files[i].refs = 1;
        return &files[i];
    }
    return 0;
}

void shared_ref(struct shared_file *file) { file->refs++; }

void shared_release(struct shared_file *file) {
    if (!--file->refs) release_buffer(file->data, file->size);
}

long shared_resize(struct shared_file *file, unsigned size) {
    if (size > LX_SHARED_LIMIT) return -27;
    if (size == file->size) return 0;
    if (file->mappings) return -16;
    u8 *next = 0;
    if (size) {
        next = file_buffer(size);
        if (!next) return -12;
        memset(next, 0, (size + PAGE - 1) & ~(PAGE - 1));
        unsigned keep = size < file->size ? size : file->size;
        if (keep) memcpy(next, file->data, keep);
    }
    release_buffer(file->data, file->size);
    file->data = next;
    file->size = size;
    return 0;
}

long shared_size(const struct shared_file *file) { return file->size; }

long shared_seek(struct shared_file *file, long distance, unsigned whence) {
    unsigned base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = file->position; break;
    case 2: base = file->size; break;
    default: return -22;
    }
    if (distance < -(long)base || distance > (long)LX_SHARED_LIMIT - base) return -22;
    file->position = base + distance;
    return file->position;
}

long shared_transfer(struct shared_file *file, u8 *bytes, unsigned count, int write) {
    if (!count) return 0;
    if (write && file->position + count > file->size) {
        long status = shared_resize(file, file->position + count);
        if (status < 0) return status;
    }
    if (file->position >= file->size) return 0;
    if (count > file->size - file->position) count = file->size - file->position;
    if (write) memcpy(file->data + file->position, bytes, count);
    else memcpy(bytes, file->data + file->position, count);
    file->position += count;
    return count;
}

u64 shared_root(unsigned cpu) { return (u64)aperture[cpu] | 3; }

static void publish(unsigned cpu, unsigned bank, unsigned slot, int visible) {
    struct mapping *m = &maps[bank][slot];
    for (unsigned i = 0; i < m->count; i++)
        aperture[cpu][slot + i] = visible ? ((u64)m->tables[i] | 3) : 0;
}

void shared_switch(unsigned bank) {
    if (mapped_bank == bank) return;
    if (mapped_bank < NATIVE_TASKS) {
        for (unsigned i = 0; i < map_end[mapped_bank]; i++) publish(smp_cpu(), mapped_bank, i, 0);
    }
    for (unsigned i = 0; i < map_end[bank]; i++) publish(smp_cpu(), bank, i, 1);
    mapped_bank = bank;
}

long shared_map(unsigned bank, struct shared_file *file, u64 bytes, u64 offset, u64 prot) {
    if (!bytes || bytes > LX_SHARED_LIMIT || offset > LX_SHARED_LIMIT ||
        (offset & (PAGE - 1)) || (prot != 1 && prot != 3)) return -22;
    bytes = (bytes + PAGE - 1) & ~(u64)(PAGE - 1);
    if (offset + bytes > ((file->size + PAGE - 1) & ~(PAGE - 1))) return -22;
    const unsigned count = (bytes + TABLE_SPAN - 1) / TABLE_SPAN;
    unsigned slot = 0;
    for (unsigned i = 0; i < map_end[bank]; i++) {
        if (!maps[bank][i].file) continue;
        if (slot + count <= i) break;
        slot = i + maps[bank][i].count;
    }
    if (slot + count > MAP_BLOCKS) return -12;
    struct mapping *m = &maps[bank][slot];
    m->tables = (u64 (*)[512])file_buffer(count * PAGE);
    if (!m->tables) return -12;

    const u64 permissions = 3 | (1UL << 10) | (3UL << 8) | (1UL << 6) |
                            (1UL << 53) | (1UL << 54) | (prot == 1 ? 1UL << 7 : 0);
    for (unsigned i = 0; i < count; i++) {
        for (unsigned j = 0; j < 512; j++) {
            u64 at = i * TABLE_SPAN + j * PAGE;
            m->tables[i][j] = at < bytes ? ((u64)file->data + offset + at) | permissions : 0;
        }
    }
    shared_ref(file);
    file->mappings++;
    m->file = file;
    if (slot >= map_end[bank]) map_end[bank] = slot + 1;
    m->bytes = bytes;
    m->count = count;
    for (unsigned cpu=0;cpu<2;cpu++)
        if (mapped_banks[cpu]==bank) publish(cpu,bank,slot,1);
    sync_image();
    return LX_SHARED_BASE + slot * TABLE_SPAN;
}

static void unmap(unsigned bank, unsigned slot) {
    struct mapping *m = &maps[bank][slot];
    for (unsigned cpu=0;cpu<2;cpu++)
        if (mapped_banks[cpu]==bank) publish(cpu,bank,slot,0);
    sync_image();
    release_buffer((u8 *)m->tables, m->count * PAGE);
    m->file->mappings--;
    shared_release(m->file);
    m->file = 0;
    m->count = 0;
    while (map_end[bank] && !maps[bank][map_end[bank] - 1].file) map_end[bank]--;
}

long shared_unmap(unsigned bank, u64 address, u64 bytes) {
    if (!bytes || bytes > LX_SHARED_LIMIT || address < LX_SHARED_BASE ||
        address >= LX_SHARED_BASE + LX_SHARED_APERTURE ||
        (address - LX_SHARED_BASE) % TABLE_SPAN) return -22;
    unsigned slot = (address - LX_SHARED_BASE) / TABLE_SPAN;
    struct mapping *m = &maps[bank][slot];
    if (!m->file || m->bytes != ((bytes + PAGE - 1) & ~(u64)(PAGE - 1))) return -22;
    unmap(bank, slot);
    return 0;
}

void shared_clear(unsigned bank) {
    for (unsigned i = 0; i < map_end[bank]; i++)
        if (maps[bank][i].file) unmap(bank, i);
}
