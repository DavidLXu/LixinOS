#include "native.h"

#define DIR 0xffffffffu
static int inside(const char *path, const char *base) {
    unsigned n = 0;
    while (base[n] && base[n] == path[n])
        n++;
    return !base[n] && (!path[n] || path[n] == '/' || (n == 1 && base[0] == '/'));
}
static long fail(int slot, char *mail, const char *s) {
    batch_abort(slot);
    do {
        *mail++ = *s;
    } while (*s++);
    return -1;
}
long file_operation(int slot, const char *cwd, u64 value, char *mail) {
    unsigned op = value & 255;
    int flag = value >> 8;
    char *other = mail + 513;
    if (op == 7) {
        batch_abort(slot);
        return 0;
    }
    if (batch_stale(slot))
        return fail(slot, mail, "filesystem changed during transaction; retry");
    if (op == 0)
        return batch_begin(slot) ? 0 : fail(slot, mail, "filesystem busy");
    if (op == 6)
        return batch_commit(slot) ? 0 : fail(slot, mail, "could not commit transaction");
    if (value >> 8 > 1 || !canonical_path(mail))
        return fail(slot, mail, "invalid path");
    if ((op == 4 || op == 5) && proc_path(other))
        return fail(slot, mail, "read-only filesystem");
    if (proc_path(mail)) {
        struct file item, old;
        int found = vfs_lookup(mail, &item);
        if (op == 8)
            return found;
        if (op == 4 && found && item.size != DIR && canonical_path(other)) {
            if (flag && batch_lookup(slot, other, &old))
                return 0;
            return batch_set(slot, other, &item) ? 0 : fail(slot, mail, "could not copy file");
        }
        return fail(slot, mail, "read-only filesystem");
    }
    struct file f, target, empty = {0, 0}, dir = {0, DIR};
    int exists = batch_lookup(slot, mail, &f);
    if (op == 8)
        return exists;
    if (!mail[1])
        return fail(slot, mail,
                    op == 1 ? "already exists" : "cannot remove the working directory or root");
    char parent[513];
    unsigned len = strlen(mail);
    while (len > 1 && mail[len - 1] != '/')
        len--;
    if (len > 1)
        len--;
    memcpy(parent, mail, len);
    parent[len] = 0;
    if (!batch_lookup(slot, parent, &target) || target.size != DIR)
        return fail(slot, mail, "not a directory");
    if (op == 1 && exists && (!flag || f.size != DIR))
        return fail(slot, mail, flag ? "not a directory" : "already exists");
    int ok = 0;
    if (op == 1)
        ok = exists ? flag && f.size == DIR : batch_set(slot, mail, &dir);
    if (op == 2)
        ok = batch_set(slot, mail, exists ? &f : &empty);
    if (op == 3) {
        char child[513];
        if (!exists || (f.size == DIR) != flag)
            return fail(slot, mail, "wrong file type or missing path");
        if (flag && inside(cwd, mail))
            return fail(slot, mail, "cannot remove the working directory or root");
        if (flag && batch_child(slot, mail, 0, child) != -1)
            return fail(slot, mail, "directory not empty");
        ok = batch_set(slot, mail, 0);
    }
    if (op == 9) {
        if (exists || !canonical_path(other))
            return fail(slot,mail,"invalid or occupied path record");
        struct file record={(const u8 *)other,strlen(other)};
        ok=batch_set(slot,mail,&record);
    }
    if (op == 4 || op == 5) {
        if (!exists || !canonical_path(other) || inside(mail, other) || inside(other, mail))
            return fail(slot, mail, "invalid source or target");
        int occupied = batch_lookup(slot, other, &target);
        if (flag && occupied)
            return 0;
        if (occupied && (target.size == DIR || f.size == DIR))
            return fail(slot, mail, "destination exists");
        if (op == 4)
            ok = f.size != DIR && batch_set(slot, other, &f);
        else
            ok = !(f.size == DIR && inside(cwd, mail)) && batch_move(slot, mail, other);
    }
    return ok ? 0
              : fail(slot, mail, disk_full() ? "filesystem full" : "filesystem operation failed");
}

/* Existing libgit2 user program. Limit mutations to this repository's .git
 * directory and publish only after the foreground process exits successfully. */
struct message {
    int length, number;
    char path[513], other[513];
    u8 bytes[16384];
};
long git_call(int slot, const char *meta, unsigned op) {
    struct message *m = (struct message *)0x443d8000UL;
    struct file f;
    if (!*meta)
        return -13;
    if (op == 7)
        return *(volatile u32 *)0x09010000;
    if (!canonical_path(m->path))
        return -22;
    if (((op >= 3 && op <= 6) || op == 9) && !inside(m->path, meta))
        return -13;
    int found = batch_lookup(slot, m->path, &f);
    switch (op) {
    case 0:
        if (!found)
            return -2;
        m->length = f.size;
        m->number = disk_generation() + 1;
        return 0;
    case 1:
        if (!found || f.size != DIR)
            return -20;
        return batch_child(slot, m->path, (unsigned)m->number, m->other) == -1;
    case 2:
        if (!found)
            return -2;
        if (f.size == DIR)
            return -21;
        if (f.size > sizeof(m->bytes))
            return -27;
        memcpy(m->bytes, f.data, f.size);
        return f.size;
    case 8:
    case 9: {
        if (!found || f.size == DIR) return found ? -21 : -2;
        unsigned off = m->number, n = m->length;
        if (m->number < 0 || n > sizeof(m->bytes)) return -22;
        if (op == 8) {
            if (off >= f.size) return 0;
            if (n > f.size - off) n = f.size - off;
            memcpy(m->bytes, f.data + off, n);
            return n;
        }
        if (off > NATIVE_FILE_LIMIT - n) return -27;
        unsigned size = f.size > off+n ? f.size : off+n;
        u8 *buf = file_buffer(size);
        if (!buf) return -12;
        memcpy(buf, f.data, f.size);
        if (off > f.size) memset(buf + f.size, 0, off-f.size);
        memcpy(buf + off, m->bytes, n);
        struct file item = {buf, size};
        int ok = batch_set(slot, m->path, &item);
        release_buffer(buf, size);
        return ok ? 0 : -5;
    }
    case 3:
    case 4: {
        if (op == 4 && found)
            return -17;
        if (op == 3 && (unsigned)m->length > sizeof(m->bytes))
            return -27;
        struct file item = {m->bytes, op == 4 ? DIR : (unsigned)m->length};
        return batch_set(slot, m->path, &item) ? 0 : -5;
    }
    case 5: {
        if (!found || (f.size == DIR) != !!m->number)
            return -2;
        char child[513];
        if (f.size == DIR && batch_child(slot, m->path, 0, child) != -1)
            return -90;
        return batch_set(slot, m->path, 0) ? 0 : -5;
    }
    case 6:
        if (!found || f.size == DIR || !canonical_path(m->other) || !inside(m->other, meta))
            return -13;
        return batch_set(slot, m->other, &f) && batch_set(slot, m->path, 0) ? 0 : -5;
    default:
        return -22;
    }
}
long git_finish(int slot, long status) {
    struct message *m = (struct message *)0x443d8000UL;
    if (disk_full()) {
        batch_abort(slot);
        serial_text("git: filesystem full\n");
        return 1;
    }
    if (status || (unsigned)m->length > sizeof(m->bytes)) {
        batch_abort(slot);
        return status ? status : 1;
    }
    if (!batch_commit(slot))
        return 1;
    for (int i = 0; i < m->length; i++) {
        if (m->bytes[i] == '\n')
            serial_put('\r');
        serial_put(m->bytes[i]);
    }
    return 0;
}
