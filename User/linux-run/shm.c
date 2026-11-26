// Named shared-memory files use the same LixinOS backing as Wayland memfds.
// The name, each open file description, and each shared mapping have separate lifetimes.
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct shm_node {
    struct shm_node *next;
    char *name;
    unsigned refs, inode, mode;
    int backing;
};
struct shm_handle {
    struct shm_node *node;
    unsigned refs;
    int flags;
    off_t position;
};
static struct shm_node *names;
static unsigned next_inode;

int shm_path(const char *path) {
    return !strncmp(path, "/dev/shm", 8) && (!path[8] || path[8] == '/');
}
static struct shm_node **lookup(const char *path) {
    struct shm_node **link = &names;
    while (*link && strcmp((*link)->name, path))
        link = &(*link)->next;
    return link;
}
static void release(struct shm_node *node) {
    if (--node->refs)
        return;
    close(node->backing);
    free(node->name);
    free(node);
}
int shm_open_file(const char *path, unsigned flags, unsigned mode, struct shm_handle **out) {
    if (!path[8] || !path[9])
        return -EISDIR;
    if (strchr(path + 9, '/') || !strcmp(path + 9, ".") || !strcmp(path + 9, ".."))
        return -ENOENT;
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    if ((flags & O_ACCMODE) == 3 ||
        (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_CLOEXEC | O_LARGEFILE | O_NOFOLLOW | O_TRUNC |
                   O_APPEND | O_NONBLOCK)))
        return -EINVAL;
    struct shm_node **link = lookup(path), *node = *link;
    if (node && (flags & O_EXCL) && (flags & O_CREAT))
        return -EEXIST;
    if (!node && !(flags & O_CREAT))
        return -ENOENT;
    if (node && (((flags & O_ACCMODE) != O_RDONLY && !(node->mode & 0200)) ||
                 ((flags & O_ACCMODE) != O_WRONLY && !(node->mode & 0400))))
        return -EACCES;
    struct shm_handle *h = calloc(1, sizeof *h);
    if (!h)
        return -ENOMEM;
    if (!node) {
        node = calloc(1, sizeof *node);
        if (!node || !(node->name = strdup(path))) {
            free(node);
            free(h);
            return -ENOMEM;
        }
        node->backing = syscall(SYS_memfd_create, path + 9, 1);
        if (node->backing < 0) {
            int e = errno;
            if (tracing) {
                int used = 0;
                for (int i = 0; i < GUEST_FDS; ++i)
                    used += fcntl(i, F_GETFD) >= 0;
                fprintf(stderr,
                        "linux-run: shm native memfd create failed errno=%d, native fds=%d\n", e,
                        used);
            }
            free(node->name);
            free(node);
            free(h);
            return -e;
        }
        node->refs = 1; // linked name
        node->mode = mode & 0777;
        node->inode = ++next_inode;
        *link = node;
    } else if (flags & O_TRUNC) {
        if ((flags & O_ACCMODE) == O_RDONLY || ftruncate(node->backing, 0)) {
            int e = (flags & O_ACCMODE) == O_RDONLY ? EACCES : errno;
            free(h);
            return -e;
        }
    }
    ++node->refs;
    h->node = node;
    h->refs = 1;
    h->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    *out = h;
    return 0;
}
void shm_retain(struct shm_handle *h) {
    if (h)
        ++h->refs;
}
void shm_drop(struct shm_handle *h) {
    if (h && !--h->refs) {
        release(h->node);
        free(h);
    }
}
int shm_remove(const char *path) {
    struct shm_node **p = lookup(path), *n = *p;
    if (!n)
        return -ENOENT;
    *p = n->next;
    free(n->name);
    n->name = NULL;
    release(n);
    return 0;
}
static long node_stat(struct shm_node *n, struct stat *st) {
    if (fstat(n->backing, st))
        return -errno;
    st->st_dev = 0x7368;
    st->st_ino = n->inode;
    st->st_mode = S_IFREG | n->mode;
    st->st_nlink = !!n->name;
    st->st_uid = namespace_id(0);
    st->st_gid = namespace_id(1);
    return 0;
}
long shm_stat_name(const char *path, struct stat *st) {
    if (!strcmp(path, "/dev/shm") || !strcmp(path, "/dev/shm/")) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFDIR | 01777;
        st->st_nlink = 2;
        return 0;
    }
    struct shm_node *n = *lookup(path);
    return n ? node_stat(n, st) : -ENOENT;
}
long shm_stat_handle(struct shm_handle *h, struct stat *st) { return node_stat(h->node, st); }
int shm_raw(struct shm_handle *h) { return h->node->backing; }
int shm_flags(struct shm_handle *h) { return h->flags; }
long shm_io(struct shm_handle *h, u64 addr, size_t len, int write_op, off_t *at) {
    int mode = h->flags & O_ACCMODE;
    if (write_op ? mode == O_RDONLY : mode == O_WRONLY)
        return -EBADF;
    void *p = guest_ptr(addr, len, write_op ? PROT_READ : PROT_WRITE);
    if (len && !p)
        return -EFAULT;
    int fd = shm_raw(h);
    off_t where = at ? *at : h->position;
    if (write_op && (h->flags & O_APPEND))
        where = lseek(fd, 0, SEEK_END);
    if (where < 0 || lseek(fd, where, SEEK_SET) < 0)
        return -EINVAL;
    ssize_t n = write_op ? write(fd, p, len) : read(fd, p, len);
    if (n < 0)
        return -errno;
    if (!at)
        h->position = where + n;
    return n;
}
long shm_seek(struct shm_handle *h, off_t distance, int whence) {
    int fd = shm_raw(h);
    if (whence == SEEK_CUR && lseek(fd, h->position, SEEK_SET) < 0)
        return -errno;
    off_t n = lseek(fd, distance, whence);
    if (n < 0)
        return -errno;
    return h->position = n;
}
