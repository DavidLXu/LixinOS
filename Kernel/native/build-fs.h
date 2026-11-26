/* In a build VM, index immutable snapshot records by name. A transaction copies
 * only the index and appends replacement records. Scratch writes grow in place
 * until a checkpoint packs all live records into the other arena. */
static struct build_view build_live;
static u8 *arena;
static u64 cursor;
static int scratch_dirty, build_indexed;
static unsigned data_len(u32 n) { return n == DIR_SIZE ? 0 : n; }
static int build_ensure(struct build_ref *r) {
    if (!r->lazy)
        return 1;
    u8 *p = r->p;
    u32 n = word(p), sz = word(p + 4);
    if (sz == DIR_SIZE || !sz) {
        r->lazy = 0;
        return 1;
    }
    u8 *dst = p + 16 + n;
    if (!read_slot_range(active_slot, r->disk_off, dst, sz) || hash(dst, sz) != r->disk_crc)
        return 0;
    r->lazy = 0;
    return 1;
}
static int build_ref_cmp(const struct build_ref *a, const struct build_ref *b) {
    u32 al = word(a->p), bl = word(b->p);
    int c = memcmp(a->p + 16, b->p + 16, al < bl ? al : bl);
    return c ? c : (al < bl ? -1 : al > bl);
}
static void build_sift(struct build_ref *items, u32 i, u32 n) {
    struct build_ref v = items[i];
    for (u32 child = 2 * i + 1; child < n; child = 2 * i + 1) {
        if (child + 1 < n && build_ref_cmp(&items[child], &items[child + 1]) < 0)
            child++;
        if (build_ref_cmp(&v, &items[child]) >= 0)
            break;
        items[i] = items[child];
        i = child;
    }
    items[i] = v;
}
static void build_sort(struct build_view *v) {
    int sorted = 1;
    for (u32 i = 1; i < v->count; i++) {
        if (build_ref_cmp(&v->items[i - 1], &v->items[i]) > 0) {
            sorted = 0;
            break;
        }
    }
    if (sorted)
        return;
    for (u32 i = v->count / 2; i; i--)
        build_sift(v->items, i - 1, v->count);
    for (u32 n = v->count; n > 1;) {
        n--;
        struct build_ref t = v->items[0];
        v->items[0] = v->items[n];
        v->items[n] = t;
        build_sift(v->items, 0, n);
    }
}
static unsigned lower_bound(struct build_view *v, const char *name, unsigned n) {
    unsigned lo = 0, hi = v->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        u8 *p = v->items[mid].p;
        unsigned m = word(p);
        int c = memcmp(p + 16, name, n < m ? n : m);
        if (c < 0 || (!c && m < n))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static int build_equal(struct build_view *v, unsigned i, const char *path, unsigned n) {
    return i < v->count && word(v->items[i].p) == n && !memcmp(v->items[i].p + 16, path, n);
}
static struct file build_file(u8 *p) {
    return (struct file){p + 16 + word(p), word(p + 4), word(p + 8), word(p + 12)};
}
static int build_find(struct build_view *v, const char *path, struct file *f) {
    unsigned n = strlen(path), i = lower_bound(v, path, n);
    if (!build_equal(v, i, path, n) || !build_ensure(&v->items[i]))
        return 0;
    *f = build_file(v->items[i].p);
    return 1;
}
static void build_init_tables(void) {
    build_live.items = (struct build_ref *)BUILD_INDICES;
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
        build_transactions[i].id = -1;
        build_transactions[i].view.items = build_live.items + (i + 1) * 1048576;
    }
    disk_select_task(0);
}
static int load_build_indexed(const u8 *head, unsigned slot, u32 size, u32 sec_off, u32 idx_bytes,
                              u32 idx_crc, u32 voff, u32 vlen) {
    u64 idx_rounded = ((u64)idx_bytes + 511) & ~511UL;
    if (idx_rounded > BUILD_ARENA_BYTES)
        return 0;
    u8 *idx = staged.data;
    for (u64 pos = 0; pos < idx_rounded;) {
        u32 n = idx_rounded - pos > (1u << 20) ? (1u << 20) : idx_rounded - pos;
        if (!transfer(0, 2 + (u64)slot * (SLOT_BYTES / 512) + sec_off + pos / 512, idx + pos, n))
            return 0;
        pos += n;
    }
    if (hash(idx, idx_bytes) != idx_crc)
        return 0;
    u32 count = word(idx), pos = 8;
    if (count > 1048576)
        return 0;
    build_init_tables();
    build_live.count = 0;
    arena = live.data;
    u64 off = 4;
    for (u32 j = 0; j < count; j++) {
        if (pos + 32 > idx_bytes)
            return 0;
        u32 plen = idx[pos] | ((u32)idx[pos + 1] << 8);
        u32 sz = word(idx + pos + 4);
        u32 birth = word(idx + pos + 8);
        u32 mod = word(idx + pos + 12);
        u32 boff = word(idx + pos + 16);
        u32 bcrc = word(idx + pos + 20);
        pos += 32;
        if (!plen || plen > 512 || plen > idx_bytes - pos || idx[pos] != '/')
            return 0;
        u32 bytes = data_len(sz);
        if (sz != DIR_SIZE && (sz > NATIVE_FILE_LIMIT || boff > size || sz > size - boff))
            return 0;
        if (off + 16 + plen + bytes > size)
            return 0;
        u8 *p = arena + off;
        store_word(p, plen);
        store_word(p + 4, sz);
        store_word(p + 8, birth);
        store_word(p + 12, mod);
        memcpy(p + 16, idx + pos, plen);
        build_live.items[j] =
            (struct build_ref){p, bytes, boff, bcrc, (sz != DIR_SIZE && sz > 0) ? 1 : 0};
        off += 16 + plen + bytes;
        pos += plen;
    }
    if (pos != idx_bytes || off + vlen > size)
        return 0;
    build_live.count = count;
    build_sort(&build_live);
    for (u32 j = 1; j < count; j++) {
        if (!build_ref_cmp(&build_live.items[j - 1], &build_live.items[j]))
            return 0;
    }
    unsigned root_i = lower_bound(&build_live, "/", 1);
    if (!build_equal(&build_live, root_i, "/", 1) ||
        word(build_live.items[root_i].p + 4) != DIR_SIZE)
        return 0;
    if (vlen && !read_slot_range(slot, voff, arena + off, vlen))
        return 0;
    cursor = size;
    build_live.versions = (struct file){arena + off, vlen};
    build_live.bytes = size;
    live.size = size;
    live.count = count;
    live.timed = 1;
    live.buckets = 0;
    ordered = 0;
    live.end = off;
    active_slot = slot;
    generation = word(head + 8) | (u64)word(head + 12) << 32;
    build_indexed = 1;
    return 1;
}
static int build_index(void) {
    if (!live.timed)
        return 0;
    build_init_tables();
    build_live.count = 0;
    arena = live.data;
    cursor = live.size;
    u64 off = 4;
    for (unsigned j = 0; j < live.count; j++) {
        u8 *p = arena + off;
        unsigned n = word(p), bytes = data_len(word(p + 4));
        build_live.items[j] = (struct build_ref){p, bytes, 0, 0, 0};
        off += 16 + n + bytes;
    }
    build_live.count = live.count;
    build_sort(&build_live);
    build_live.versions = (struct file){arena + off, live.size - off};
    build_live.bytes = live.size;
    build_indexed = 1;
    return 1;
}
static long build_child(struct build_view *v, const char *path, u64 index, char *out) {
    char prefix[514];
    unsigned n = strlen(path);
    memcpy(prefix, path, n);
    if (n > 1)
        prefix[n++] = '/';
    for (unsigned i = lower_bound(v, prefix, n); i < v->count; i++) {
        u8 *p = v->items[i].p;
        unsigned len = word(p);
        if (len < n || memcmp(p + 16, prefix, n))
            break;
        if (len == n)
            continue;
        unsigned j = n;
        while (j < len && p[16 + j] != '/')
            j++;
        if (j != len) {
            /* Skip all descendants with this prefix. */
            char after[514];
            memcpy(after, p + 16, j);
            after[j] = '0'; /* ASCII immediately following '/'. */
            i = lower_bound(v, after, j + 1) - 1;
            continue;
        }
        if (index--)
            continue;
        memcpy(out, p + 16 + n, len - n);
        out[len - n] = 0;
        return word(p + 4) == DIR_SIZE ? -2 : (long)word(p + 4);
    }
    return -1;
}
/* Write payload before the alternate header, as on ordinary volumes. */
static int build_pack(struct build_view *v, int write_disk) {
#ifndef LIXINOS_BOOT_DISK
    detach_open_files(0);
#endif
    u8 *dst = (u8 *)((u64)arena == BUILD_FIRST ? BUILD_SECOND : BUILD_FIRST);
    u64 off = 4;
    store_word(dst, v->count);
    for (unsigned i = 0; i < v->count; i++) {
        if (!build_ensure(&v->items[i]))
            return 0;
        u8 *p = v->items[i].p;
        unsigned len = 16 + word(p) + data_len(word(p + 4));
        memcpy(dst + off, p, len);
        off += len;
    }
    u64 voff = off;
    memcpy(dst + off, v->versions.data, v->versions.size);
    u64 size = off + v->versions.size;
    if (size != v->bytes || size > SLOT_BYTES - 512)
        return 0;
    if (write_disk) {
        u8 head[512] = {0};
        memcpy(head, "LIXINFS3", 8);
        u64 seq = generation + 1, sector = 1 + (1 - active_slot) * (SLOT_BYTES / 512);
        u64 rounded = (size + 511) & ~511UL;
        memset(dst + size, 0, rounded - size);
        u32 running = hash_cont(2166136261u, dst, 4);
        /* Build LXIDX001 in free arena space immediately after the rounded payload. */
        u8 *idx = dst + rounded;
        u64 idx_cap = (SLOT_BYTES - 512 > rounded) ? (SLOT_BYTES - 512 - rounded) : 0;
        u64 idx_pos = 8;
        int make_idx = idx_cap >= 8;
        if (make_idx) {
            store_word(idx, v->count);
            store_word(idx + 4, 1); /* INDEX_FLAG_HASHES */
        }
        u64 roff = 4;
        for (unsigned i = 0; i < v->count; i++) {
            u8 *p = dst + roff;
            u32 plen = word(p), sz = word(p + 4), birth = word(p + 8), mod = word(p + 12);
            u32 bytes = data_len(sz);
            running = hash_cont(running, p, 16 + plen);
            u32 h_before = running, bcrc = 0;
            if (bytes) {
                bcrc = hash(p + 16 + plen, bytes);
                running = hash_cont(running, p + 16 + plen, bytes);
            } else if (sz != DIR_SIZE) {
                bcrc = 2166136261u;
            }
            if (make_idx) {
                if (idx_pos + 32 + plen <= idx_cap) {
                    u8 *e = idx + idx_pos;
                    e[0] = plen;
                    e[1] = plen >> 8;
                    e[2] = e[3] = 0;
                    store_word(e + 4, sz);
                    store_word(e + 8, birth);
                    store_word(e + 12, mod);
                    store_word(e + 16, sz == DIR_SIZE ? 0 : (u32)(roff + 16 + plen));
                    store_word(e + 20, bcrc);
                    store_word(e + 24, h_before);
                    store_word(e + 28, running);
                    memcpy(e + 32, p + 16, plen);
                    idx_pos += 32 + plen;
                } else {
                    make_idx = 0;
                }
            }
            roff += 16 + plen + bytes;
        }
        if (v->versions.size)
            running = hash_cont(running, dst + voff, v->versions.size);
        store_word(head + 8, seq);
        store_word(head + 12, seq >> 32);
        store_word(head + 16, size);
        store_word(head + 20, running);
        store_word(head + 24, hash(head, 24));
        u64 write_end = rounded;
        if (make_idx) {
            u64 idx_rounded = (idx_pos + 511) & ~511UL;
            if (rounded + idx_rounded <= SLOT_BYTES - 512) {
                memset(idx + idx_pos, 0, idx_rounded - idx_pos);
                memcpy(head + 28, "LXIDX001", 8);
                store_word(head + 36, (u32)(rounded / 512));
                store_word(head + 40, (u32)idx_pos);
                store_word(head + 44, hash(idx, (u32)idx_pos));
                store_word(head + 48, (u32)voff);
                store_word(head + 52, (u32)v->versions.size);
                store_word(head + 56, hash(head, 56));
                write_end = rounded + idx_rounded;
            }
        }
        for (u64 pos = 0; pos < write_end;) {
            u32 n = write_end - pos > (1 << 20) ? 1 << 20 : write_end - pos;
            if (!transfer(1, sector + 1 + pos / 512, dst + pos, n))
                return 0;
            pos += n;
        }
        if (!transfer(4, 0, 0, 0) || !transfer(1, sector, head, 512) || !transfer(4, 0, 0, 0)) {
            writable = 0;
            return 0;
        }
        active_slot = 1 - active_slot;
        generation = seq;
        scratch_dirty = 0;
    }
    build_revision++;
    arena = dst;
    cursor = size;
    v->versions.data = dst + voff;
    off = 4;
    for (unsigned i = 0; i < v->count; i++) {
        u8 *p = dst + off;
        unsigned bytes = data_len(word(p + 4));
        v->items[i] = (struct build_ref){p, bytes, 0, 0, 0};
        off += 16 + word(p) + bytes;
    }
    return 1;
}
static int build_begin(int id) {
    if (cursor + NATIVE_FILE_LIMIT + 1024 > BUILD_ARENA_BYTES && !build_pack(&build_live, 0))
        return 0;
    owner = id;
    transaction->revision = build_revision;
    struct build_ref *items = build_staged.items;
    build_staged = build_live;
    build_staged.items = items;
    memcpy(items, build_live.items, build_live.count * sizeof(*items));
    dirty = full = needs_checkpoint = 0;
    return 1;
}
static int scratch_name(const char *name) { return !strncmp(name, "/scratch/", 9) || !strncmp(name,"/tmp/",5); }
static int build_set(const char *path, const struct file *f) {
    unsigned n = strlen(path), at = lower_bound(&build_staged, path, n);
    int exists = build_equal(&build_staged, at, path, n);
    unsigned parent = n;
    while (parent > 1 && path[parent - 1] != '/')
        parent--;
    if (parent > 1)
        parent--;
    unsigned parent_index = lower_bound(&build_staged, path, parent);
    int parent_exists = build_equal(&build_staged, parent_index, path, parent) &&
                        word(build_staged.items[parent_index].p + 4) == DIR_SIZE;
    /* A subtree rename removes its old parent before its descendants. */
    if (f && !parent_exists)
        return 0;
    u32 now = *(volatile u32 *)0x09010000, birth = now;
    u64 old = 0, next = f ? 16 + n + data_len(f->size) : 0;
    if (exists) {
        struct file prior = build_file(build_staged.items[at].p);
        if (f && (f->size == DIR_SIZE) != (prior.size == DIR_SIZE))
            return 0;
        birth = prior.birth;
        old = 16 + n + data_len(prior.size);
    } else if (!f || build_staged.count >= 1048576)
        return 0;
    int membership_change = parent_exists && exists != !!f;
    unsigned parent_record = membership_change ? 16 + parent : 0;
    if (build_staged.bytes - old + next > SLOT_BYTES - 512 ||
        cursor + next + parent_record > BUILD_ARENA_BYTES) {
        full = 1;
        return 0;
    }
    if (f) {
        unsigned bytes = data_len(f->size);
        if (bytes > NATIVE_FILE_LIMIT)
            return 0;
        u8 *p = arena + cursor;
        cursor += next;
        store_word(p, n);
        store_word(p + 4, f->size);
        store_word(p + 8, birth);
        store_word(p + 12, now);
        memcpy(p + 16, path, n);
        memcpy(p + 16 + n, f->data, bytes);
        if (!exists) {
            memmove(build_staged.items + at + 1, build_staged.items + at,
                    (build_staged.count - at) * sizeof(*build_staged.items));
            build_staged.count++;
        }
        build_staged.items[at] = (struct build_ref){p, bytes, 0, 0, 0};
    } else {
        build_staged.count--;
        memmove(build_staged.items + at, build_staged.items + at + 1,
                (build_staged.count - at) * sizeof(*build_staged.items));
    }
    if (membership_change) {
        /* Copy the parent's timestamp too, so an aborted batch leaves it intact. */
        parent_index = lower_bound(&build_staged, path, parent);
        u8 *parent_copy = arena + cursor;
        memcpy(parent_copy, build_staged.items[parent_index].p, parent_record);
        store_word(parent_copy + 12, now);
        build_staged.items[parent_index].p = parent_copy;
        cursor += parent_record;
    }
    build_staged.bytes = build_staged.bytes - old + next;
#ifndef LIXINOS_BOOT_DISK
    if (exists)
        detach_open_files(path);
#endif
    dirty = 1;
    if (!scratch_name(path))
        needs_checkpoint = 1;
    return 1;
}
static int build_commit(void) {
    if (needs_checkpoint && !build_pack(&build_staged, 1)) {
        return 0;
    }
    if (!needs_checkpoint)
        scratch_dirty = 1;
    struct build_view old = build_live;
    build_revision++;
    build_live = build_staged;
    build_staged = old;
    return 1;
}
int disk_checkpoint(void) {
    return !build_volume || !scratch_dirty || (owner < 0 && writable && build_pack(&build_live, 1));
}
int scratch_write(const char *path, unsigned offset, const void *data, unsigned n) {
    if (!build_volume || !scratch_name(path))
        return -1;
    if (!agent_path_allowed(path, 1) || !disk_access(0,path,2) || owner >= 0 || !writable || offset > NATIVE_FILE_LIMIT - n)
        return 0;
    unsigned len = strlen(path), i = lower_bound(&build_live, path, len);
    if (!build_equal(&build_live, i, path, len))
        return 0;
    struct build_ref *r = build_live.items + i;
    if (!build_ensure(r))
        return 0;
    unsigned old = word(r->p + 4), end = offset + n;
    if (old == DIR_SIZE)
        return 0;
    unsigned size = end > old ? end : old;
    if (build_live.bytes + size - old > SLOT_BYTES - 512)
        return 0;
#ifndef LIXINOS_BOOT_DISK
    detach_open_files(path);
#endif
    if (size > r->capacity) {
        unsigned cap = r->capacity < 4096 ? 4096 : r->capacity;
        while (cap < size)
            cap = cap > NATIVE_FILE_LIMIT / 2 ? NATIVE_FILE_LIMIT : cap * 2;
        if (cursor + 16 + len + cap > BUILD_ARENA_BYTES && !build_pack(&build_live, 0))
            return 0;
        u8 *p = arena + cursor;
        cursor += 16 + len + cap;
        memcpy(p, r->p, 16 + len + old);
        r->p = p;
        r->capacity = cap;
    }
    u8 *dst = r->p + 16 + len;
    if (offset > old)
        memset(dst + old, 0, offset - old);
    memcpy(dst + offset, data, n);
    store_word(r->p + 4, size);
    store_word(r->p + 12, *(volatile u32 *)0x09010000);
    build_revision++;
    build_live.bytes += size - old;
    scratch_dirty = 1;
    return 1;
}
