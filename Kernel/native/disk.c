#include "native.h"
#include "identity.h"
#include "build-memory.h"

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
#ifndef LIXINOS_BOOT_DISK
    boot_splash_tick();
#endif
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

static u64 slot_bytes = SNAPSHOT_SLOT_BYTES;
#define SLOT_BYTES slot_bytes
int build_volume;
u64 disk_capacity(void) { return slot_bytes - 512; }
#define DIR_SIZE 0xffffffffu
/* Live and staged snapshots occupy consecutive privileged RAM regions. */
struct snapshot {
    u8 *data;
    u32 size, count, end, timed, buckets;
};
static struct snapshot live = {(u8 *)0x80000000UL}, staged = {(u8 *)0x8c000000UL};
struct build_ref {
    u8 *p;
    u32 capacity, disk_off, disk_crc;
    u8 lazy;
};
struct build_view {
    struct build_ref *items;
    u32 count;
    u64 bytes;
    struct file versions;
};
struct disk_transaction {
    int id, changed, out_of_space, durable;
    u64 revision;
    struct build_view view;
};
static struct disk_transaction ordinary = {.id = -1}, *transaction = &ordinary;
#define owner (transaction->id)
#define dirty (transaction->changed)
#define full (transaction->out_of_space)
#define needs_checkpoint (transaction->durable)
#define build_staged (transaction->view)
#ifndef LIXINOS_BOOT_DISK
/* A task can be preempted between begin and rename/commit. Each index is
 * private; publication checks its revision before replacing the live index. */
static struct disk_transaction build_transactions[NATIVE_TASKS];
static u64 build_revision;
void disk_select_task(unsigned id) {
    if (build_volume)
        transaction = &build_transactions[id];
}
int batch_stale(int id) {
    return build_volume && owner == id && transaction->revision != build_revision;
}
#endif
int disk_full(void) { return full; }
static unsigned active_slot;
static u64 generation;

u64 disk_generation(void) { return generation; }

static u32 word(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void store_word(u8 *p, u32 value) {
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}
static u32 hash_cont(u32 h, const u8 *p, u32 n) {
    while (n >= 4) {
        h = (h ^ p[0]) * 16777619u;
        h = (h ^ p[1]) * 16777619u;
        h = (h ^ p[2]) * 16777619u;
        h = (h ^ p[3]) * 16777619u;
        p += 4;
        n -= 4;
    }
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
static u32 hash(const u8 *p, u32 n) { return hash_cont(2166136261u, p, n); }
static int read_slot_range(unsigned slot, u64 payload_off, u8 *dst, u32 len) {
    if (!len)
        return 1;
    u64 base = 2 + (u64)slot * (SLOT_BYTES / 512);
    u64 sec = payload_off / 512;
    u32 head = payload_off % 512;
    u8 tmp[512];
    if (head) {
        if (!transfer(0, base + sec, tmp, 512))
            return 0;
        u32 take = 512 - head;
        if (take > len)
            take = len;
        memcpy(dst, tmp + head, take);
        dst += take;
        len -= take;
        sec++;
    }
    u32 aligned_len = len & ~511u;
    for (u32 pos = 0; pos < aligned_len;) {
        u32 n = aligned_len - pos > (1u << 20) ? (1u << 20) : aligned_len - pos;
        if (!transfer(0, base + sec + pos / 512, dst + pos, n))
            return 0;
        pos += n;
    }
    dst += aligned_len;
    len -= aligned_len;
    sec += aligned_len / 512;
    if (len) {
        if (!transfer(0, base + sec, tmp, 512))
            return 0;
        memcpy(dst, tmp, len);
    }
    return 1;
}
static int matches(const u8 *p, const char *name, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (p[i] != (u8)name[i])
            return 0;
    return 1;
}
static int parse_idx_header(const u8 *h, u32 size, u32 *sec_off, u32 *idx_bytes, u32 *idx_crc,
                            u32 *voff, u32 *vlen) {
    if (!matches(h + 28, "LXIDX001", 8) || word(h + 56) != hash(h, 56))
        return 0;
    u32 so = word(h + 36), ib = word(h + 40), ic = word(h + 44), vo = word(h + 48),
        vl = word(h + 52);
    u64 payload_secs = ((u64)size + 511) / 512;
    u64 idx_secs = ((u64)ib + 511) / 512;
    if (so < payload_secs || ib < 8 || 1UL + so + idx_secs > SLOT_BYTES / 512 || vo > size ||
        vl > size - vo || vl > (1u << 20))
        return 0;
    *sec_off = so;
    *idx_bytes = ib;
    *idx_crc = ic;
    *voff = vo;
    *vlen = vl;
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

/* The two offset tables occupy the gap between syscall scratch and the
 * snapshot arenas. Zero is unused because entry records start at byte four. */
#define MAX_BUCKETS (0x700000u / sizeof(u32))
static u32 *table(struct snapshot *s) {
    return (u32 *)((s->data == (u8 *)0x80000000UL || s->data == (u8 *)BUILD_FIRST) ? 0x47200000UL
                                                                                   : 0x47900000UL);
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
static void base_file_stats(u64 *stats) {
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

static int disk_access(int staging,const char *path,unsigned bits);
#include "build-fs.h"
u32 disk_usage(void) { return build_volume ? build_live.bytes : live.size; }
void file_stats(u64 *stats) {
    if (!build_volume) {
        base_file_stats(stats);
        return;
    }
    stats[0] = stats[1] = stats[2] = 0;
    for (unsigned i = 0; i < build_live.count; i++) {
        unsigned size = word(build_live.items[i].p + 4);
        if (size == DIR_SIZE)
            stats[1]++;
        else {
            stats[0]++;
            stats[2] += size;
        }
    }
}
static int raw_find(int staging,const char *path,struct file *f) {
    if (build_volume) return build_find(staging?&build_staged:&build_live,path,f);
    return find(staging?&staged:&live,path,f);
}
static int metadata(int staging,struct file *f) {
    if (!raw_find(staging,LX_PERMISSIONS,f)) {f->data=0;f->size=0;return 1;}
    return f->size<=LX_PERM_LIMIT;
}
static int disk_access(int staging,const char *path,unsigned bits) {
    unsigned slot=task_active();
    if (slot>=16 || !id_uid(slot)) return 1;
    struct file meta,target;
    if (!metadata(staging,&meta)) return 0;
    int kind=raw_find(staging,path,&target)?target.size==DIR_SIZE:-1;
    return id_path(slot,meta.data,meta.size,path,strlen(path),kind,bits);
}
int lookup(const char *path, struct file *f) {
    return agent_path_allowed(path,0) && disk_access(0,path,4) && raw_find(0,path,f);
}
int disk_can_execute(const char *path) {return disk_access(0,path,1);}
int batch_lookup(int id,const char *path,struct file *f) {
    return id==owner && !batch_stale(id) && agent_path_allowed(path,0) &&
        disk_access(1,path,4) && raw_find(1,path,f);
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
    u32 sec_off, idx_bytes, idx_crc, voff, vlen;
    if (matches(head, "LIXINFS3", 8) &&
        parse_idx_header(head, size, &sec_off, &idx_bytes, &idx_crc, &voff, &vlen)) {
#ifdef LIXINOS_BOOT_DISK
        u64 idx_rounded = ((u64)idx_bytes + 511) & ~511UL;
        if (idx_rounded <= SLOT_BYTES) {
            int ok = 1;
            for (u64 pos = 0; pos < idx_rounded;) {
                u32 n = idx_rounded - pos > (1u << 20) ? (1u << 20) : idx_rounded - pos;
                if (!transfer(0, 2 + (u64)slot * (SLOT_BYTES / 512) + sec_off + pos / 512,
                              staged.data + pos, n)) {
                    ok = 0;
                    break;
                }
                pos += n;
            }
            if (ok && hash(staged.data, idx_bytes) == idx_crc) {
                u32 count = word(staged.data), pos = 8;
                const char *want = "/boot/kernel.elf";
                u32 want_len = 16;
                for (u32 i = 0; i < count && pos + 32 <= idx_bytes; i++) {
                    u32 plen = staged.data[pos] | ((u32)staged.data[pos + 1] << 8);
                    u32 sz = word(staged.data + pos + 4);
                    u32 birth = word(staged.data + pos + 8);
                    u32 mod = word(staged.data + pos + 12);
                    u32 boff = word(staged.data + pos + 16);
                    u32 bcrc = word(staged.data + pos + 20);
                    pos += 32;
                    if (!plen || plen > 512 || plen > idx_bytes - pos)
                        break;
                    if (plen == want_len && sz != DIR_SIZE && sz <= NATIVE_FILE_LIMIT &&
                        boff <= size && sz <= size - boff &&
                        matches(staged.data + pos, want, want_len)) {
                        u8 *out = live.data;
                        store_word(out, 2);
                        u32 o = 4;
                        store_word(out + o, 1);
                        store_word(out + o + 4, DIR_SIZE);
                        store_word(out + o + 8, 0);
                        store_word(out + o + 12, 0);
                        out[o + 16] = '/';
                        o += 17;
                        store_word(out + o, want_len);
                        store_word(out + o + 4, sz);
                        store_word(out + o + 8, birth);
                        store_word(out + o + 12, mod);
                        memcpy(out + o + 16, want, want_len);
                        if (read_slot_range(slot, boff, out + o + 16 + want_len, sz) &&
                            hash(out + o + 16 + want_len, sz) == bcrc) {
                            live.size = o + 16 + want_len + sz;
                            live.count = 2;
                            live.timed = 1;
                            live.buckets = 0;
                            ordered = 0;
                            live.end = live.size;
                            active_slot = slot;
                            generation = word(head + 8) | (u64)word(head + 12) << 32;
                            return 1;
                        }
                        break;
                    }
                    pos += plen;
                }
            }
        }
#else
        if (build_volume &&
            load_build_indexed(head, slot, size, sec_off, idx_bytes, idx_crc, voff, vlen))
            return 1;
#endif
    }
    u64 total = ((u64)size + 511) & ~511UL;
    for (u64 pos = 0; pos < total;) {
        u32 n = total - pos > (1u << 20) ? (1u << 20) : total - pos;
        if (!transfer(0, 2 + (u64)slot * (SLOT_BYTES / 512) + pos / 512, live.data + pos, n))
            return 0;
        pos += n;
    }
    u32 digest = 2166136261u;
    for (u32 pos = 0; pos < size;) {
#ifndef LIXINOS_BOOT_DISK
        boot_splash_tick();
#endif
        u32 chunk = size - pos;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        digest = hash_cont(digest, live.data + pos, chunk);
        pos += chunk;
    }
    if (digest != word(head + 20))
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
    if (!find(&live, "/", &root) || root.size != DIR_SIZE)
        return 0;
    live.end = offset;
    active_slot = slot;

    generation = word(head + 8) | (u64)word(head + 12) << 32;
    return 1;
}

/* QEMU virt reserves the first two MiB of RAM for the flattened device tree.
 * Both boot stages check the actual RAM extent before touching a build volume. */
static unsigned fdt_word(const unsigned char *p) {
    return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
int build_memory_present(void) {
    const unsigned char *dt = (const unsigned char *)0x40000000UL;
    unsigned size = fdt_word(dt + 4), strings = fdt_word(dt + 12), start = fdt_word(dt + 8);
    if (fdt_word(dt) != 0xd00dfeed || size < 40 || size > (2u << 20) || strings >= size ||
        start > size || fdt_word(dt + 36) > size - start)
        return 0;
    unsigned end = start + fdt_word(dt + 36), depth = 0, memory = 0;
    for (unsigned i = start; i + 4 <= end;) {
        unsigned token = fdt_word(dt + i);
        i += 4;
        if (token == 1) {
            unsigned name = i;
            while (i < end && dt[i])
                i++;
            if (i == end)
                return 0;
            depth++;
            if (depth == 2)
                memory = i - name >= 7 && !memcmp(dt + name, "memory@", 7);
            i = (i + 4) & ~3u;
        } else if (token == 2) {
            if (!depth)
                return 0;
            if (depth-- == 2)
                memory = 0;
        } else if (token == 3) {
            if (end - i < 8)
                return 0;
            unsigned len = fdt_word(dt + i), key = fdt_word(dt + i + 4);
            i += 8;
            if (len > end - i || key >= size - strings)
                return 0;
            if (memory && depth == 2 && len == 16 && size - strings - key >= 4 &&
                !memcmp(dt + strings + key, "reg", 4) && !fdt_word(dt + i) &&
                fdt_word(dt + i + 4) == 0x40000000 &&
                (((u64)fdt_word(dt + i + 8) << 32) | fdt_word(dt + i + 12)) >= BUILD_RAM)
                return 1;
            i = (i + len + 3) & ~3u;
        } else if (token != 4)
            return 0;
    }
    return 0;
}

int mount_disk(void) {
    u8 layout[512], heads[2][512];
    if (!init_blk() || !transfer(0, 0, layout, 512))
        return 0;
    if (matches(layout, "LIXBLD01", 8)) {
        u64 sectors = REG(0x100) | ((u64)REG(0x104) << 32);
        if (!build_memory_present() ||
            (sectors != 1 + 2 * (3 * GIB / 512) + 130 && sectors != 1 + 2 * (4 * GIB / 512) + 130))
            return 0;
        slot_bytes = (sectors - 131) * 256;
        build_volume = 1;
        live.data = (u8 *)BUILD_FIRST;
        staged.data = (u8 *)BUILD_SECOND;
    } else if (!matches(layout, "LIXDISK4", 8))
        return 0;
    for (unsigned i = 8; i < 512; i++)
        if (layout[i])
            return 0;
    for (unsigned i = 0; i < 2; i++)
        if (!transfer(0, 1 + i * (SLOT_BYTES / 512), heads[i], 512))
            return 0;
    u64 a = ((u64)word(heads[0] + 12) << 32) | word(heads[0] + 8);
    u64 b = ((u64)word(heads[1] + 12) << 32) | word(heads[1] + 8);
    if (!load_snapshot(heads[b > a], b > a) && !load_snapshot(heads[b <= a], b <= a))
        return 0;
#ifndef LIXINOS_BOOT_DISK
    if (build_volume && !build_indexed)
        return build_index();
#endif
    return 1;
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

long directory_entry(const char *path, u64 index, char *name) {
    return agent_path_allowed(path, 0) && disk_access(0,path,5) ? (build_volume ? build_child(&build_live, path, index, name)
                                                       : list_entry(&live, path, index, name))
                                       : -1;
}
long batch_child(int id, const char *path, u64 index, char *name) {
    return id == owner && !batch_stale(id) && agent_path_allowed(path, 0) && disk_access(1,path,5)
               ? (build_volume ? build_child(&build_staged, path, index, name)
                               : list_entry(&staged, path, index, name))
               : -1;
}
int batch_begin(int id) {
    if (id < 0 || owner >= 0 || !writable || !flush_supported || generation == (u64)-1)
        return 0;
    if (build_volume)
        return build_begin(id);
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
    if (owner == id) {
        owner = -1;
    }
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
static int batch_set_raw(int id, const char *path, const struct file *value) {
    if (owner != id || batch_stale(id) || !canonical_path(path) || !path[1])
        return 0;
    u32 n = 0;
    while (path[n])
        n++;
    if (!value && n >= 3 && matches((const u8 *)path + n - 3, ".py", 3) && !edit_revision(id,path,0))
        return 0;
    if (build_volume)
        return build_set(path, value);
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
    return build_volume ? build_live.versions
                        : (struct file){live.data + live.end, live.size - live.end};
}
int stage_versions(int slot, const u8 *bytes, unsigned size) {
    if (build_volume) {
        if (owner != slot || batch_stale(slot) || size > (1u << 20) ||
            cursor + size > BUILD_ARENA_BYTES ||
            build_staged.bytes - build_staged.versions.size + size > SLOT_BYTES - 512)
            return 0;
        build_staged.bytes = build_staged.bytes - build_staged.versions.size + size;
        memcpy(arena + cursor, bytes, size);
        build_staged.versions = (struct file){arena + cursor, size};
        cursor += size;
        dirty = needs_checkpoint = 1;
        return 1;
    }
    if (owner != slot || size > (1u << 20) || size > SLOT_BYTES - 512 - staged.end) return 0;
    if (size) memcpy(staged.data + staged.end, bytes, size);
    staged.size = staged.end + size;
    dirty = 1;
    return 1;
}
int batch_commit(int id) {
    if (owner != id)
        return 0;
    if (batch_stale(id)) {
        batch_abort(id);
        return 0;
    }
    owner = -1;
    if (!dirty)
        return 1;
    if (build_volume)
        return build_commit();
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
        u32 bytes = rounded - pos > (1u << 20) ? (1u << 20) : rounded - pos;
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
static int save_mode(int id,const char *path,const struct lx_mode *mode) {
    struct file meta;
    if (!metadata(1,&meta)) return 0;
    unsigned n=strlen(path),cap=meta.size+16+n;
    if (!mode) {
        struct lx_mode dummy;
        long found=id_find(meta.data,meta.size,path,n,&dummy,0);
        if (found<=0) return !found;
    }
    u8 *buf=file_buffer(cap);
    if (!buf) return 0;
    long size=id_encode(meta.data,meta.size,path,n,mode,buf,cap);
    struct file result={buf,size<0?0:size};
    int previous=needs_checkpoint;
    int ok=size>=0 && batch_set_raw(id,LX_PERMISSIONS,&result);
    if (build_volume && scratch_name(path)) needs_checkpoint=previous;
    release_buffer(buf,cap);
    return ok;
}
int batch_set(int id,const char *path,const struct file *value) {
    if (id!=owner || !canonical_path(path) || !agent_path_allowed(path,1) ||
        !disk_access(1,path,value?2:8)) return 0;
    unsigned slot=task_active();
    struct file prior;
    int create=value && slot<16 && id_uid(slot) && !raw_find(1,path,&prior);
    if (!batch_set_raw(id,path,value)) return 0;
    if ((strlen(path)==sizeof(LX_PERMISSIONS)-1 && !memcmp(path,LX_PERMISSIONS,sizeof(LX_PERMISSIONS)-1))) return 1;
    if (!value || create) {
        struct lx_mode m={id_uid(slot),id_gid(slot),(value && value->size==DIR_SIZE?0777:0666)&~id_mask(slot)};
        if (!save_mode(id,path,value?&m:0)) {batch_abort(id);return 0;}
    }
    return 1;
}
long disk_mode(const char *path,unsigned op,unsigned *w) {
    unsigned slot=task_active(),n=strlen(path);
    struct file f,meta;
    struct lx_mode m;
    if (op==16 || op==17) {
        if (raw_find(0,path,&f)) return (w[1] || op==17)?-17:0;
        if (!agent_path_allowed(path,1) || !disk_access(0,path,2)) return -13;
        int id=0x7ffffffe;
        if (!batch_begin(id)) return -16;
        struct file created={0,op==17?DIR_SIZE:0};
        struct lx_mode attrs={id_uid(slot),id_gid(slot),w[0]&0777&~id_mask(slot)};
        if (batch_set(id,path,&created) && save_mode(id,path,&attrs) && batch_commit(id)) return 0;
        batch_abort(id);return -5;
    }
    if (!agent_path_allowed(path,op==13||op==14) || !disk_access(0,path,0)) return -13;
    if (!raw_find(0,path,&f)) return -2;
    if (!metadata(0,&meta) || !id_mode(meta.data,meta.size,path,n,f.size==DIR_SIZE,&m)) return -5;
    if (op==12) {w[0]=m.uid;w[1]=m.gid;w[2]=m.mode;return 0;}
    if (op==15) return !(w[0]&~7u) && disk_access(0,path,w[0])?0:-13;
    if ((strlen(path)==sizeof(LX_PERMISSIONS)-1 && !memcmp(path,LX_PERMISSIONS,sizeof(LX_PERMISSIONS)-1))) return -13;
    long res=id_modify(slot,&m,op,w[0],w[1]);
    if (res) return res;
    int id=0x7ffffffe;
    if (!batch_begin(id)) return -16;
    if (save_mode(id,path,&m) && batch_commit(id)) return 0;
    batch_abort(id);return -5;
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
    if (!src || !dst || owner != id || batch_stale(id))
        return 0;
    u32 offset = 4;
    for (unsigned i = build_volume ? lower_bound(&build_live, source, src) : 0;
         i < (build_volume ? build_live.count : live.count); i++) {
        struct entry e;
        if (build_volume) {
            if (!build_ensure(&build_live.items[i]))
                return 0;
            u8 *p = build_live.items[i].p;
            struct file f = build_file(p);
            e = (struct entry){p + 16, f.data, word(p), f.size, f.birth, f.modified};
            if (e.length < src || memcmp(e.name, source, src))
                break;
        } else if (!next_entry(&live, &offset, &e))
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
        struct file meta;
        struct lx_mode mode;
        if (!metadata(0,&meta) || !id_mode(meta.data,meta.size,from,e.length,e.size==DIR_SIZE,&mode)) return 0;
        if (!batch_set(id, from, 0) || !batch_set(id, to, &f) || !save_mode(id,to,&mode))
            return 0;
        if ((build_volume || staged.timed) && batch_lookup(id, to, &f)) {
            u8 *stamp = (u8 *)f.data - (dst + e.length - src) - 8;
            store_word(stamp, e.birth);
            store_word(stamp + 4, e.modified);
        }
    }
    return 1;
}
#endif
