/* In a build VM, index immutable snapshot records by name. A transaction copies
 * only the index and appends replacement records. Scratch writes grow in place
 * until a checkpoint packs all live records into the other arena. */
static struct build_view build_live;
static u8 *arena;
static u64 cursor;
static int scratch_dirty;
static unsigned data_len(u32 n) { return n == DIR_SIZE ? 0 : n; }
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
    if (!build_equal(v, i, path, n))
        return 0;
    *f = build_file(v->items[i].p);
    return 1;
}
static int build_index(void) {
    if (!live.timed)
        return 0;
    build_live.items = (struct build_ref *)BUILD_INDICES;
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
        build_transactions[i].id = -1;
        build_transactions[i].view.items = build_live.items + (i + 1) * 1048576;
    }
    disk_select_task(0);
    build_live.count = 0;
    arena = live.data;
    cursor = live.size;
    unsigned off = 4;
    for (unsigned j = 0; j < live.count; j++) {
        u8 *p = arena + off;
        unsigned n = word(p), bytes = data_len(word(p + 4));
        unsigned i = lower_bound(&build_live, (const char *)p + 16, n);
        memmove(build_live.items + i + 1, build_live.items + i,
                (build_live.count - i) * sizeof(*build_live.items));
        build_live.items[i] = (struct build_ref){p, bytes};
        build_live.count++;
        off += 16 + n + bytes;
    }
    build_live.versions = (struct file){arena + off, live.size - off};
    build_live.bytes = live.size;
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
    u8 *dst = (u8 *)((u64)arena == BUILD_FIRST ? BUILD_SECOND : BUILD_FIRST);
    u64 off = 4;
    store_word(dst, v->count);
    for (unsigned i = 0; i < v->count; i++) {
        u8 *p = v->items[i].p;
        unsigned len = 16 + word(p) + data_len(word(p + 4));
        memcpy(dst + off, p, len);
        off += len;
    }
    memcpy(dst + off, v->versions.data, v->versions.size);
    u64 size = off + v->versions.size;
    if (size != v->bytes || size > SLOT_BYTES - 512)
        return 0;
    if (write_disk) {
        u8 head[512] = {0};
        memcpy(head, "LIXINFS3", 8);
        u64 seq = generation + 1, sector = 1 + (1 - active_slot) * (SLOT_BYTES / 512);
        store_word(head + 8, seq);
        store_word(head + 12, seq >> 32);
        store_word(head + 16, size);
        store_word(head + 20, hash(dst, size));
        store_word(head + 24, hash(head, 24));
        u64 rounded = (size + 511) & ~511UL;
        memset(dst + size, 0, rounded - size);
        for (u64 pos = 0; pos < rounded;) {
            u32 n = rounded - pos > (1 << 20) ? 1 << 20 : rounded - pos;
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
    v->versions.data = dst + off;
    off = 4;
    for (unsigned i = 0; i < v->count; i++) {
        u8 *p = dst + off;
        unsigned bytes = data_len(word(p + 4));
        v->items[i] = (struct build_ref){p, bytes};
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
static int scratch_name(const char *name) { return !strncmp(name, "/scratch/", 9); }
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
        build_staged.items[at] = (struct build_ref){p, bytes};
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
    if (!agent_path_allowed(path, 1) || owner >= 0 || !writable || offset > NATIVE_FILE_LIMIT - n)
        return 0;
    unsigned len = strlen(path), i = lower_bound(&build_live, path, len);
    if (!build_equal(&build_live, i, path, len))
        return 0;
    struct build_ref *r = build_live.items + i;
    unsigned old = word(r->p + 4), end = offset + n;
    if (old == DIR_SIZE)
        return 0;
    unsigned size = end > old ? end : old;
    if (build_live.bytes + size - old > SLOT_BYTES - 512)
        return 0;
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
