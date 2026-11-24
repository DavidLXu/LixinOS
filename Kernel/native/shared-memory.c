#include "native.h"

#define PAGE 4096U
#define TABLE_SPAN (512 * PAGE)
#define MAP_TABLES (LX_SHARED_LIMIT / TABLE_SPAN)
#define TABLE_POOL 128
#define SHARED_FILES (2 * NATIVE_TASKS)

struct shared_file {
    unsigned refs, mappings, size, position;
    u8 *data;
};
static struct shared_file files[SHARED_FILES];

struct mapping {
    struct shared_file *file;
    unsigned bytes, count;
    u8 tables[MAP_TABLES];
};
static struct mapping maps[NATIVE_TASKS][LX_SHARED_MAPS];
static u64 aperture[512] __attribute__((aligned(PAGE)));
static u64 page_tables[TABLE_POOL][512] __attribute__((aligned(PAGE)));
static u8 tables_used[TABLE_POOL];
static unsigned mapped_bank = NATIVE_TASKS;

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

u64 shared_root(void) { return (u64)aperture | 3; }

static void publish(unsigned bank, unsigned slot, int visible) {
    struct mapping *m = &maps[bank][slot];
    for (unsigned i = 0; i < m->count; i++)
        aperture[slot * MAP_TABLES + i] = visible ? ((u64)page_tables[m->tables[i]] | 3) : 0;
}

void shared_switch(unsigned bank) {
    if (mapped_bank == bank) return;
    if (mapped_bank < NATIVE_TASKS) {
        for (unsigned i = 0; i < LX_SHARED_MAPS; i++) publish(mapped_bank, i, 0);
    }
    for (unsigned i = 0; i < LX_SHARED_MAPS; i++) publish(bank, i, 1);
    mapped_bank = bank;
}

long shared_map(unsigned bank, struct shared_file *file, u64 bytes, u64 offset, u64 prot) {
    if (!bytes || bytes > LX_SHARED_LIMIT || offset > LX_SHARED_LIMIT ||
        (offset & (PAGE - 1)) || (prot != 1 && prot != 3)) return -22;
    bytes = (bytes + PAGE - 1) & ~(u64)(PAGE - 1);
    if (offset + bytes > ((file->size + PAGE - 1) & ~(PAGE - 1))) return -22;
    unsigned slot = 0;
    while (slot < LX_SHARED_MAPS && maps[bank][slot].file) slot++;
    if (slot == LX_SHARED_MAPS) return -12;
    struct mapping *m = &maps[bank][slot];
    const unsigned count = (bytes + TABLE_SPAN - 1) / TABLE_SPAN;
    unsigned found = 0;
    for (unsigned i = 0; i < TABLE_POOL && found < count; i++)
        if (!tables_used[i]) m->tables[found++] = i;
    if (found != count) return -12;

    const u64 permissions = 3 | (1UL << 10) | (3UL << 8) | (1UL << 6) |
                            (1UL << 53) | (1UL << 54) | (prot == 1 ? 1UL << 7 : 0);
    for (unsigned i = 0; i < count; i++) {
        unsigned table = m->tables[i];
        tables_used[table] = 1;
        for (unsigned j = 0; j < 512; j++) {
            u64 at = i * TABLE_SPAN + j * PAGE;
            page_tables[table][j] = at < bytes ? ((u64)file->data + offset + at) | permissions : 0;
        }
    }
    shared_ref(file);
    file->mappings++;
    m->file = file;
    m->bytes = bytes;
    m->count = count;
    publish(bank, slot, 1);
    sync_image();
    return LX_SHARED_BASE + slot * LX_SHARED_LIMIT;
}

static void unmap(unsigned bank, unsigned slot) {
    struct mapping *m = &maps[bank][slot];
    if (mapped_bank == bank) {
        publish(bank, slot, 0);
        sync_image();
    }
    for (unsigned i = 0; i < m->count; i++) tables_used[m->tables[i]] = 0;
    m->file->mappings--;
    shared_release(m->file);
    m->file = 0;
    m->count = 0;
}

long shared_unmap(unsigned bank, u64 address, u64 bytes) {
    if (!bytes || bytes > LX_SHARED_LIMIT || address < LX_SHARED_BASE ||
        address >= LX_SHARED_BASE + LX_SHARED_MAPS * LX_SHARED_LIMIT ||
        (address - LX_SHARED_BASE) % LX_SHARED_LIMIT) return -22;
    unsigned slot = (address - LX_SHARED_BASE) / LX_SHARED_LIMIT;
    struct mapping *m = &maps[bank][slot];
    if (!m->file || m->bytes != ((bytes + PAGE - 1) & ~(u64)(PAGE - 1))) return -22;
    unmap(bank, slot);
    return 0;
}

void shared_clear(unsigned bank) {
    for (unsigned i = 0; i < LX_SHARED_MAPS; i++)
        if (maps[bank][i].file) unmap(bank, i);
}
