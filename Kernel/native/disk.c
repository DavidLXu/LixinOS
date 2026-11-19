#include "native.h"

/* Modern virtio-mmio block device, queue zero. The kernel is single-core
 * and submits one read at a time. All DMA addresses are identity mapped. */
static volatile u32 *const mmio = (volatile u32 *)0x0a000000;
#define REG(offset) mmio[(offset) / 4]
struct descriptor {
    u64 address;
    u32 length;
    u16 flags, next;
};
static struct descriptor desc[8] __attribute__((aligned(16)));
static struct {
    u16 flags, index, ring[8], event;
} avail __attribute__((aligned(2)));
static volatile struct {
    u16 flags, index;
    struct {
        u32 id, size;
    } ring[8];
    u16 event;
} used __attribute__((aligned(4)));
static struct {
    u32 type, reserved;
    u64 sector;
} request;
static volatile u8 status;
static void barrier(void) { asm volatile("dsb sy" ::: "memory"); }

static void queue_address(unsigned reg, const volatile void *p) {
    REG(reg) = (u32)(u64)p;
    REG(reg + 4) = (u64)p >> 32;
}

static int writable, flush_supported;
static int init_blk(void) {
    if (REG(0) != 0x74726976 || REG(4) != 2 || REG(8) != 2)
        return 0;
    REG(0x70) = 0;
    while (REG(0x70)) {
    }
    REG(0x70) = 3;
    REG(0x14) = 1;
    if (!(REG(0x10) & 1))
        return 0;
    REG(0x14) = 0;
    u32 features = REG(0x10);
    writable = !(features & (1u << 5));
    flush_supported = !!(features & (1u << 9));
    REG(0x24) = 0;
    REG(0x20) = features & ((1u << 5) | (1u << 9));
    REG(0x24) = 1;
    REG(0x20) = 1; /* VIRTIO_F_VERSION_1 */
    REG(0x70) = 11;
    if (!(REG(0x70) & 8))
        return 0;
    REG(0x30) = 0;
    if (REG(0x44) || REG(0x34) < 8)
        return 0;
    REG(0x38) = 8;
    queue_address(0x80, desc);
    queue_address(0x90, &avail);
    queue_address(0xa0, &used);
    REG(0x44) = 1;
    REG(0x70) = 15;
    return 1;
}

static int transfer(u32 type, u64 sector, void *data, u32 bytes) {
    u16 seq = avail.index;
    request.type = type;
    request.sector = sector;
    status = 255;
    desc[0] = (struct descriptor){(u64)&request, 16, 1, bytes ? 1 : 2};
    desc[1] = (struct descriptor){(u64)data, bytes, type == 0 ? 3 : 1, 2};
    desc[2] = (struct descriptor){(u64)&status, 1, 2, 0};
    avail.ring[seq & 7] = 0;
    barrier();
    avail.index = seq + 1;
    barrier();
    REG(0x50) = 0;
    while (used.index == seq) {
    }
    barrier();
    REG(0x64) = REG(0x60);
    return used.ring[seq & 7].id == 0 && used.ring[seq & 7].size == (type == 0 ? bytes + 1 : 1) &&
           status == 0;
}

#define SLOT_BYTES SNAPSHOT_SLOT_BYTES
#define DIR_SIZE 0xffffffffu
/* Live and staged snapshots occupy consecutive privileged RAM regions. */
struct snapshot {
    u8 *data;
    u32 size, count, end, timed, buckets;
};
static struct snapshot live = {(u8 *)0x80000000UL}, staged = {(u8 *)0x8c000000UL};
static int owner = -1, dirty, full;
int disk_full(void) { return full; }
static unsigned active_slot;
static u64 generation;

u64 disk_generation(void) { return generation; }
u32 disk_usage(void) { return live.size; }
static u32 word(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static u32 hash(const u8 *p, u32 n) {
    u32 h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
static int matches(const u8 *p, const char *name, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (p[i] != (u8)name[i])
            return 0;
    return 1;
}

struct entry {
    const u8 *name, *data;
    u32 length, size, birth, modified;
};
static int next_entry(struct snapshot *view, u32 *offset, struct entry *e) {
    if (view->size - *offset < 8)
        return 0;
    e->length = word(view->data + *offset);
    e->size = word(view->data + *offset + 4);
    *offset += 8;
    e->birth = e->modified = 0;
    if (view->timed) {
        if (view->size - *offset < 8)
            return 0;
        e->birth = word(view->data + *offset);
        e->modified = word(view->data + *offset + 4);
        *offset += 8;
    }
    if (!e->length || e->length > 512 || e->length > view->size - *offset)
        return 0;
    e->name = view->data + *offset;
    if (*e->name != '/')
        return 0;
    *offset += e->length;
    e->data = view->data + *offset;
    if (e->size != DIR_SIZE) {
        if (e->size > NATIVE_FILE_LIMIT || e->size > view->size - *offset)
            return 0;
        *offset += e->size;
    }
    return 1;
}

void file_stats(u64 *stats) {
    u32 off = 4;
    stats[0] = stats[1] = stats[2] = 0;
    for (unsigned i = 0; i < live.count; i++) {
        struct entry e;
        if (!next_entry(&live, &off, &e))
            break;
        if (e.size == DIR_SIZE)
            stats[1]++;
        else {
            stats[0]++;
            stats[2] += e.size;
        }
    }
}

/* The two offset tables occupy the gap between syscall scratch and the
 * snapshot arenas. Zero is unused because entry records start at byte four. */
#define MAX_BUCKETS (0x700000u / sizeof(u32))
static u32 *table(struct snapshot *s) {
    return (u32 *)(s->data == (u8 *)0x80000000UL ? 0x47200000UL : 0x47900000UL);
}
static u32 bucket(struct snapshot *s, const u8 *name, u32 n) {
    u32 *slots = table(s), b = hash(name, n) % s->buckets;
    while (slots[b]) {
        u32 at = slots[b], len = word(s->data + at);
        if (len == n && !memcmp(s->data + at + (s->timed ? 16 : 8), name, n))
            break;
        if (++b == s->buckets)
            b = 0;
    }
    return b;
}
static int index_view(struct snapshot *s) {
    if (s->buckets && s->count * 4UL < s->buckets * 3UL)
        return 1;
    u32 size = 4096, off = 4;
    while (size * 3UL <= s->count * 4UL && size < MAX_BUCKETS)
        size *= 2;
    if (size > MAX_BUCKETS)
        size = MAX_BUCKETS;
    s->buckets = size;
    u32 *slots = table(s);
    memset(slots, 0, size * sizeof(*slots));
    for (u32 i = 0; i < s->count; i++) {
        struct entry e;
        u32 start = off;
        if (!next_entry(s, &off, &e)) {
            s->buckets = 0;
            return 0;
        }
        u32 b = bucket(s, e.name, e.length);
        if (slots[b]) {
            s->buckets = 0;
            return 0;
        }
        slots[b] = start;
    }
    return 1;
}
static int find(struct snapshot *s, const char *path, struct file *f) {
    if (!index_view(s))
        return 0;
    u32 at = table(s)[bucket(s, (const u8 *)path, strlen(path))];
    struct entry e;
    if (!at || !next_entry(s, &at, &e))
        return 0;
    *f = (struct file){e.data, e.size, e.birth, e.modified};
    return 1;
}

static struct snapshot *ordered;
#ifdef LIXINOS_BOOT_DISK
int lookup(const char *path, struct file *f) { return find(&live, path, f); }
#else
/* Sorting offsets keeps directory iteration lexical even when a batch appended
 * its new entries in creation order. Only build this view when listing. */
static u32 order[1048576];
static int order_cmp(struct snapshot *view, u32 left, u32 right) {
    u8 *a = view->data + left, *b = view->data + right;
    u32 alen = word(a), blen = word(b), head = view->timed ? 16 : 8;
    int cmp = memcmp(a + head, b + head, alen < blen ? alen : blen);
    return cmp ? cmp : (int)alen - (int)blen;
}
static void sift(struct snapshot *view, u32 i, u32 n) {
    u32 value = order[i];
    for (u32 child = 2 * i + 1; child < n; child = 2 * i + 1) {
        if (child + 1 < n && order_cmp(view, order[child], order[child + 1]) < 0)
            child++;
        if (order_cmp(view, value, order[child]) >= 0)
            break;
        order[i] = order[child];
        i = child;
    }
    order[i] = value;
}
static int order_view(struct snapshot *view) {
    if (ordered == view)
        return 1;
    u32 off = 4;
    int sorted = 1;
    for (u32 i = 0; i < view->count; i++) {
        struct entry e;
        order[i] = off;
        if (!next_entry(view, &off, &e))
            return 0;
        if (i && order_cmp(view, order[i - 1], order[i]) > 0)
            sorted = 0;
    }
    if (!sorted) {
        for (u32 i = view->count / 2; i; i--)
            sift(view, i - 1, view->count);
        for (u32 n = view->count; n > 1;) {
            n--;
            u32 v = order[0];
            order[0] = order[n];
            order[n] = v;
            sift(view, 0, n);
        }
    }
    ordered = view;
    return 1;
}
int lookup(const char *path, struct file *f) { return agent_path_allowed(path,0) && find(&live, path, f); }
int batch_lookup(int id, const char *path, struct file *f) {
    return id == owner && agent_path_allowed(path,0) && find(&staged, path, f);
}
#endif
static int load_snapshot(const u8 *head, unsigned slot) {
    if ((!matches(head, "LIXINFS1", 8) && !matches(head, "LIXINFS2", 8) &&
         !matches(head, "LIXINFS3", 8)) ||
        word(head + 24) != hash(head, 24))
        return 0;
    u32 size = word(head + 16);
    if (size < 4 || size > SLOT_BYTES - 512)
        return 0;
    u32 total = (size + 511) & ~511u;
    for (u32 pos = 0; pos < total;) {
        u32 n = total - pos > 65536 ? 65536 : total - pos;
        if (!transfer(0, 2 + slot * (SLOT_BYTES / 512) + pos / 512, live.data + pos, n))
            return 0;
        pos += n;
    }
    if (hash(live.data, size) != word(head + 20))
        return 0;
    live.size = size;
    live.timed = head[7] == '3';
    u32 count = word(live.data), offset = 4;
    if (count > 1048576)
        return 0;
    for (u32 i = 0; i < count; i++) {
        struct entry e;
        if (!next_entry(&live, &offset, &e))
            return 0;
    }
    if (head[7] == '1' && offset != size)
        return 0;
    live.count = count;
    live.buckets = 0;
    ordered = 0;
    struct file root;
    if (!lookup("/", &root) || root.size != DIR_SIZE)
        return 0;
    live.end = offset;
    active_slot = slot;

    generation = word(head + 8) | (u64)word(head + 12) << 32;
    return 1;
}

int mount_disk(void) {
    u8 layout[512], heads[2][512];
    if (!init_blk() || !transfer(0, 0, layout, 512) || !matches(layout, "LIXDISK4", 8))
        return 0;
    for (unsigned i = 8; i < 512; i++)
        if (layout[i])
            return 0;
    for (unsigned i = 0; i < 2; i++)
        if (!transfer(0, 1 + i * (SLOT_BYTES / 512), heads[i], 512))
            return 0;
    u64 a = ((u64)word(heads[0] + 12) << 32) | word(heads[0] + 8);
    u64 b = ((u64)word(heads[1] + 12) << 32) | word(heads[1] + 8);
    return load_snapshot(heads[b > a], b > a) || load_snapshot(heads[b <= a], b <= a);
}

#ifndef LIXINOS_BOOT_DISK
static long list_entry(struct snapshot *view, const char *path, u64 index, char *name) {
    unsigned n = 0, offset = 4;
    while (path[n])
        n++;
    if (!order_view(view))
        return -1;
    for (u32 i = 0; i < view->count; i++) {
        struct entry e;
        offset = order[i];
        if (!next_entry(view, &offset, &e))
            break;
        if (e.length <= n || !matches(e.name, path, n))
            continue;
        unsigned start = n;
        if (n > 1 && e.name[start++] != '/')
            continue;
        unsigned end = start;
        while (end < e.length && e.name[end] != '/')
            end++;
        if (end != e.length || index-- != 0)
            continue;
        memcpy(name, e.name + start, end - start);
        name[end - start] = 0;
        return e.size == DIR_SIZE ? -2 : (long)e.size;
    }
    return -1;
}

static void store_word(u8 *p, u32 value) {
    for (int i = 0; i < 4; i++)
        p[i] = value >> (i * 8);
}

long directory_entry(const char *path, u64 index, char *name) {
    return agent_path_allowed(path,0) ? list_entry(&live, path, index, name) : -1;
}
long batch_child(int id, const char *path, u64 index, char *name) {
    return id == owner && agent_path_allowed(path,0) ? list_entry(&staged, path, index, name) : -1;
}
int batch_begin(int id) {
    if (id < 0 || owner >= 0 || !writable || !flush_supported || generation == (u64)-1)
        return 0;
    memcpy(staged.data, live.data, live.size);
    staged.size = live.size;
    staged.count = live.count;
    staged.end = live.end;
    staged.timed = live.timed;
    staged.buckets = live.buckets;
    if (live.buckets)
        memcpy(table(&staged), table(&live), 4UL * live.buckets);
    if (ordered == &staged)
        ordered = 0;
    owner = id;
    dirty = 0;
    full = 0;
    return 1;
}
void batch_abort(int id) {
    if (owner == id)
        owner = -1;
}
int canonical_path(const char *path) {
    if (path[0] != '/')
        return 0;
    if (!path[1])
        return 1;
    unsigned n = 1, start = 1;
    for (;; n++) {
        if (n > 512)
            return 0;
        if (path[n] && path[n] != '/')
            continue;
        unsigned len = n - start;
        if (!len || (path[start] == '.' && (len == 1 || (len == 2 && path[start + 1] == '.'))))
            return 0;
        if (!path[n])
            return 1;
        start = n + 1;
    }
}
// Set one entry in a private, sorted snapshot. NULL removes it. A directory
// has size UINT32_MAX. A source view can alias the staged snapshot.
#include "revision.h"
int batch_set(int id, const char *path, const struct file *value) {
    if (owner != id || !canonical_path(path) || !path[1] || !agent_path_allowed(path,1))
        return 0;
    u32 n = 0;
    while (path[n])
        n++;
    if (!value && n >= 3 && matches((const u8 *)path + n - 3, ".py", 3) && !edit_revision(id,path,0))
        return 0;
    unsigned parent = n;
    while (parent && path[parent - 1] != '/')
        parent--;
    char base[513];
    u32 base_len = parent > 1 ? parent - 1 : 1;
    memcpy(base, path, base_len);
    base[base_len] = 0;
    struct file dir;
    if (value && (!find(&staged, base, &dir) || dir.size != DIR_SIZE))
        return 0;
    if (!index_view(&staged))
        return 0;
    u32 b = bucket(&staged, (const u8 *)path, n);
    u32 first = table(&staged)[b], last = first;
    int exists = first != 0;
    u32 modified = *(volatile u32 *)0x09010000UL, birth = modified;
    if (exists) {
        struct entry e;
        if (!next_entry(&staged, &last, &e) ||
            (value && (e.size == DIR_SIZE) != (value->size == DIR_SIZE)))
            return 0;
        birth = e.birth;
    } else {
        if (!value)
            return 0;
        if (staged.count == 1048576) {
            full = 1;
            return 0;
        }
        first = last = staged.end;
    }
    u32 bytes = value && value->size != DIR_SIZE ? value->size : 0;
    if (bytes > NATIVE_FILE_LIMIT)
        return 0;
    u32 header = staged.timed ? 16 : 8;
    u32 record = value ? header + n + bytes : 0, total = staged.size - (last - first) + record;
    if (total > SLOT_BYTES - 512) {
        full = 1;
        return 0;
    }
    const u8 *src = value ? value->data : 0;
    if (bytes && (u64)src >= (u64)staged.data && (u64)src < (u64)staged.data + staged.size) {
        /* Existing records before the replaced record stay put. Records after
         * it move with the tail. Touching the replaced record keeps its data. */
        u32 offset = src - staged.data;
        if (offset >= last)
            src = staged.data + first + record + (offset - last);
    }
    memmove(staged.data + first + record, staged.data + last, staged.size - last);
    if (value) {
        store_word(staged.data + first, n);
        store_word(staged.data + first + 4, value->size);
        if (staged.timed) {
            store_word(staged.data + first + 8, birth);
            store_word(staged.data + first + 12, modified);
        }
        memcpy(staged.data + first + header, path, n);
        if (bytes) memmove(staged.data + first + header + n, src, bytes);
    }
    if (ordered == &staged)
        ordered = 0;
    if (!exists)
        table(&staged)[b] = first;
    else if (!value || record != last - first)
        staged.buckets = 0;
    staged.count += (!!value) - exists;
    staged.end = staged.end - (last - first) + record;
    staged.size = total;
    store_word(staged.data, staged.count);
    if (staged.timed && (!!value) != exists && find(&staged, base, &dir))
        store_word((u8 *)dir.data - base_len - 4, modified);
    dirty = 1;
    return 1;
}
struct file agent_versions(void) {
    return (struct file){live.data + live.end, live.size - live.end};
}
int stage_versions(int slot, const u8 *bytes, unsigned size) {
    if (owner != slot || size > (1u << 20) || size > SLOT_BYTES - 512 - staged.end) return 0;
    if (size) memcpy(staged.data + staged.end, bytes, size);
    staged.size = staged.end + size;
    dirty = 1;
    return 1;
}
int batch_commit(int id) {
    if (owner != id)
        return 0;
    owner = -1;
    if (!dirty)
        return 1;
    u32 total = staged.size;
    u8 *scratch = staged.data;
    u32 rounded = (total + 511) & ~511u;
    memset(scratch + total, 0, rounded - total);
    u8 head[512] = {0};
    memcpy(head, staged.timed ? "LIXINFS3" : staged.end < staged.size ? "LIXINFS2" : "LIXINFS1", 8);
    u64 gen = generation + 1;
    store_word(head + 8, gen);
    store_word(head + 12, gen >> 32);
    store_word(head + 16, total);
    store_word(head + 20, hash(scratch, total));
    store_word(head + 24, hash(head, 24));
    u32 sector = 1 + (1 - active_slot) * (SLOT_BYTES / 512);
    for (u32 pos = 0; pos < rounded;) {
        u32 bytes = rounded - pos > 65536 ? 65536 : rounded - pos;
        if (!transfer(1, sector + 1 + pos / 512, scratch + pos, bytes))
            return 0;
        pos += bytes;
    }
    if (!transfer(4, 0, 0, 0) || !transfer(1, sector, head, 512) || !transfer(4, 0, 0, 0)) {
        // A failed publication flush has an uncertain outcome. Remount before
        // accepting another write, so a potentially committed slot survives.
        writable = 0;
        return 0;
    }
    ordered = 0;
    struct snapshot old = live;
    live = staged;
    staged = old;
    generation = gen;
    active_slot = 1 - active_slot;
    return 1;
}
int store_file(const char *path, const u8 *data, u32 size) {
    const int single = 0x7fffffff;
    struct file file = {data, size};
    if (size > NATIVE_FILE_LIMIT || !batch_begin(single))
        return 0;
    if (batch_set(single, path, &file))
        return batch_commit(single);
    batch_abort(single);
    return 0;
}

#define HISTORY_SLOTS 65
#define HISTORY_SECTOR (1 + 2 * (SLOT_BYTES / 512))
static struct {
    u64 seq;
    char text[513];
} history[HISTORY_SLOTS];
static u64 latest_history;
void read_history(void) {
    u8 record[1024];
    for (unsigned i = 0; i < HISTORY_SLOTS; i++) {
        if (!transfer(0, HISTORY_SECTOR + 2 * i, record, sizeof(record)))
            return;
        u64 seq = word(record + 8) | (u64)word(record + 12) << 32;
        u32 len = record[16] + 256u * record[17];
        if (!matches(record, "LIXHIST1", 8) || !seq || seq % HISTORY_SLOTS != i || !len ||
            len > 512 || word(record + 18 + len) != hash(record, 18 + len))
            continue;
        unsigned j = 0;
        while (j < len && record[18 + j] >= 32 && record[18 + j] <= 126)
            j++;
        if (j != len)
            continue;
        memcpy(history[i].text, record + 18, len);
        history[i].seq = seq;
        if (seq > latest_history)
            latest_history = seq;
    }
}
const char *history_get(unsigned index) {
    u64 ordered[HISTORY_SLOTS];
    unsigned count = 0;
    for (unsigned i = 0; i < HISTORY_SLOTS; i++)
        if (history[i].seq) {
            unsigned j = count++;
            while (j && ordered[j - 1] > history[i].seq) {
                ordered[j] = ordered[j - 1];
                j--;
            }
            ordered[j] = history[i].seq;
        }
    if (count > 64 && index < 64)
        index++;
    else if (index >= 64)
        return 0;
    return index < count ? history[ordered[index] % HISTORY_SLOTS].text : 0;
}
int history_append(const char *s, unsigned len) {
    if (!len || len > 512 || !writable || !flush_supported || latest_history == (u64)-1)
        return 0;
    u8 record[1024] = {0};
    u64 seq = latest_history + 1;
    memcpy(record, "LIXHIST1", 8);
    store_word(record + 8, seq);
    store_word(record + 12, seq >> 32);
    record[16] = len;
    record[17] = len >> 8;
    memcpy(record + 18, s, len);
    store_word(record + 18 + len, hash(record, 18 + len));
    unsigned slot = seq % HISTORY_SLOTS;
    if (!transfer(1, HISTORY_SECTOR + 2 * slot, record, 1024) || !transfer(4, 0, 0, 0))
        return 0;
    history[slot].seq = latest_history = seq;
    memcpy(history[slot].text, s, len);
    history[slot].text[len] = 0;
    return 1;
}

int batch_move(int id, const char *source, const char *dest) {
    unsigned src = 0, dst = 0;
    while (source[src])
        src++;
    while (dest[dst])
        dst++;
    if (!src || !dst || owner != id)
        return 0;
    u32 offset = 4;
    for (unsigned i = 0; i < live.count; i++) {
        struct entry e;
        if (!next_entry(&live, &offset, &e))
            return 0;
        if (e.length < src || !matches(e.name, source, src) ||
            (e.length > src && e.name[src] != '/'))
            continue;
        if (dst + e.length - src > 512)
            return 0;
        char from[513], to[513];
        memcpy(from, e.name, e.length);
        from[e.length] = 0;
        memcpy(to, dest, dst);
        memcpy(to + dst, e.name + src, e.length - src);
        to[dst + e.length - src] = 0;
        struct file f = {e.data, e.size};
        unsigned to_len=dst+e.length-src;
        if (e.length>=3 && !memcmp(from+e.length-3,".py",3) &&
            !edit_revision(id,from,to_len>=3 && !memcmp(to+to_len-3,".py",3) ? to : 0)) return 0;
        if (!batch_set(id, from, 0) || !batch_set(id, to, &f))
            return 0;
        if (staged.timed && find(&staged, to, &f)) {
            u8 *stamp = (u8 *)f.data - (dst + e.length - src) - 8;
            store_word(stamp, e.birth);
            store_word(stamp + 4, e.modified);
        }
    }
    return 1;
}
#endif
