// newlib/libgit2 platform for the guest's transactional filesystem.
#include "platform.h"
#include "../console.h"
#include "alloc.h"
#include "errors.h"
#include "filter.h"
#include "hash.h"
#include "mwindow.h"
#include "oid.h"
#include "pool.h"
#include "runtime.h"
#include "settings.h"
#include "sysdir.h"
#include "thread.h"
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

int lx_libgit_init(void) {
    git_runtime_init_fn init[] = {git_allocator_global_init, git_error_global_init,
                                  git_threads_global_init,   git_oid_global_init,
                                  git_hash_global_init,      git_sysdir_global_init,
                                  git_filter_global_init,    git_mwindow_global_init,
                                  git_pool_global_init,      git_settings_global_init};
    return git_runtime_init(init, sizeof(init) / sizeof(*init)) < 0 ? -1 : 0;
}

// Git gets its own per-process EL0 arena, including when it runs as a builtin.
void *_sbrk(ptrdiff_t n) {
    static size_t used;
    volatile int *length = (volatile int *)0x443d8000;
    if (n < 0 || (size_t)n > (128u << 20) - used) {
        errno = ENOMEM;
        return (void *)-1;
    }
    int saved = *length;
    *length = used + n;
    long base = console_call(6, 10);
    *length = saved;
    if (base < 0) { errno = ENOMEM; return (void *)-1; }
    void *old = (void *)(base + used);
    used += n;
    return old;
}
void _exit(int status) { finish(status); }

#include <pwd.h>
#include <stdarg.h>
#include <git2/remote.h>

enum { LX_STAT, LX_LIST, LX_READ, LX_WRITE, LX_MKDIR, LX_REMOVE, LX_RENAME, LX_TIME, LX_PREAD, LX_PWRITE };
enum { PATH_CAP = 513, FILE_CAP = 16384 };
typedef struct {
    int length, number;
    char path[PATH_CAP], other[PATH_CAP];
    unsigned char bytes[FILE_CAP];
} Message;
static Message *const msg = (Message *)0x443d8000;
// The kernel prints this line only after it has published the disk snapshot.
void lx_confirm(const char *line) {
    msg->length = strlen(line);
    memcpy(msg->bytes, line, msg->length);
}
static char cwd[PATH_CAP];
typedef struct {
    char path[PATH_CAP];
    int offset, flags;
} Descriptor;
static Descriptor descriptors[48];
struct lx_dir {
    char path[PATH_CAP];
    int next;
    struct dirent entry;
};

void lx_start(const char *root) {
    strcpy(cwd, root);
    setenv("HOME", "/nonexistent", 1);
}
static int error(int code) {
    errno = code;
    return -1;
}

static int canonical(char *out, const char *path) {
    if (!path || !*path)
        return error(ENOENT);
    strcpy(out, *path == '/' ? "/" : cwd);
    while (*path) {
        while (*path == '/')
            ++path;
        size_t n = strcspn(path, "/");
        if (!n)
            break;
        if (n == 2 && !strncmp(path, "..", 2)) {
            char *slash = strrchr(out, '/');
            if (slash == out)
                out[1] = 0;
            else
                *slash = 0;
        } else if (!(n == 1 && *path == '.')) {
            size_t len = strlen(out);
            if (len + (len != 1) + n >= PATH_CAP)
                return error(ENAMETOOLONG);
            if (len != 1)
                out[len++] = '/';
            memcpy(out + len, path, n);
            out[len + n] = 0;
        }
        path += n;
    }
    return 0;
}
static int rpc(int op) {
    long result = console_call(6, op);
    if (result < 0)
        return error((int)-result);
    return (int)result;
}
static int call(int op, const char *path) { return canonical(msg->path, path) < 0 ? -1 : rpc(op); }
int _stat(const char *path, struct stat *st) {
    if (call(LX_STAT, path) < 0)
        return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = msg->length < 0 ? S_IFDIR | 0755 : S_IFREG | 0644;
    st->st_size = msg->length < 0 ? 0 : msg->length;
    st->st_mtime = st->st_ctime = msg->number;
    st->st_nlink = 1;
    return 0;
}
int lstat(const char *p, struct stat *s) { return _stat(p, s); }
int access(const char *p, int mode) {
    (void)mode;
    struct stat s;
    return _stat(p, &s);
}
int mkdir(const char *p, mode_t mode) {
    (void)mode;
    return call(LX_MKDIR, p);
}
int _unlink(const char *p) {
    msg->number = 0;
    return call(LX_REMOVE, p);
}
int rmdir(const char *p) {
    msg->number = 1;
    return call(LX_REMOVE, p);
}
int _rename(const char *a, const char *b) {
    if (canonical(msg->other, b) < 0)
        return -1;
    return call(LX_RENAME, a);
}
int rename(const char *a, const char *b) { return _rename(a, b); }
int _link(const char *a, const char *b) {
    (void)a;
    (void)b;
    return error(ENOTSUP);
}
int symlink(const char *a, const char *b) {
    (void)a;
    (void)b;
    return error(ENOTSUP);
}
ssize_t readlink(const char *p, char *buf, size_t n) {
    (void)p;
    (void)buf;
    (void)n;
    return error(EINVAL);
}
int chmod(const char *p, mode_t mode) {
    (void)mode;
    return access(p, 0);
}
int utimes(const char *p, const struct timeval times[2]) {
    (void)times;
    return access(p, 0);
}
char *getcwd(char *out, size_t n) {
    if (!out || strlen(cwd) >= n) {
        errno = ERANGE;
        return NULL;
    }
    return strcpy(out, cwd);
}
char *p_realpath(const char *p, char *out) {
    char tmp[PATH_CAP];
    if (canonical(tmp, p) < 0 || access(tmp, 0) < 0)
        return NULL;
    return out ? strcpy(out, tmp) : strdup(tmp);
}

static Descriptor *descriptor(int fd) {
    if (fd < 3 || fd >= 3 + (int)(sizeof(descriptors) / sizeof(*descriptors)) ||
        !descriptors[fd - 3].path[0]) {
        errno = EBADF;
        return NULL;
    }
    return &descriptors[fd - 3];
}
int _open(const char *p, int flags, ...) {
    struct stat st;
    int fd;
    for (fd = 0; fd < (int)(sizeof(descriptors) / sizeof(*descriptors)); fd++)
        if (!descriptors[fd].path[0])
            break;
    if (fd == (int)(sizeof(descriptors) / sizeof(*descriptors)))
        return error(EMFILE);
    int exists = _stat(p, &st) == 0;
    if (!exists && (!(flags & O_CREAT) || errno != ENOENT))
        return -1;
    if (exists && (flags & O_CREAT) && (flags & O_EXCL))
        return error(EEXIST);
    if (exists && S_ISDIR(st.st_mode))
        return error(EISDIR);
    if (!exists || flags & O_TRUNC) {
        msg->length = 0;
        if (call(LX_WRITE, p) < 0)
            return -1;
    }
    if (canonical(descriptors[fd].path, p) < 0)
        return -1;
    descriptors[fd].flags = flags;
    descriptors[fd].offset = 0;
    return fd + 3;
}
int _close(int fd) {
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    d->path[0] = 0;
    return 0;
}
int _fstat(int fd, struct stat *st) {
    if (fd == 1 || fd == 2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR;
        return 0;
    }
    Descriptor *d = descriptor(fd);
    return d ? _stat(d->path, st) : -1;
}
int fsync(int fd) { return descriptor(fd) ? 0 : -1; }
int _isatty(int fd) { return fd == 1 || fd == 2; }
int _lseek(int fd, int offset, int whence) {
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    struct stat st;
    if (whence == SEEK_END && _stat(d->path, &st) < 0)
        return -1;
    long pos = (long)offset + (whence == SEEK_CUR   ? d->offset
                               : whence == SEEK_END ? st.st_size
                                                    : 0);
    if (pos < 0 || pos > INT_MAX || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END))
        return error(EINVAL);
    return d->offset = (int)pos;
}
ssize_t pread(int fd, void *out, size_t n, off_t offset) {
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    if (offset < 0 || offset > INT_MAX || n > INT_MAX - (size_t)offset)
        return error(EINVAL);
    size_t done = 0;
    while (done < n) {
        int chunk = n - done > FILE_CAP ? FILE_CAP : (int)(n - done);
        msg->number = offset + done;
        msg->length = chunk;
        int got = call(LX_PREAD, d->path);
        if (got < 0) return done ? (ssize_t)done : -1;
        if (got > chunk) return error(EIO);
        memcpy((char *)out + done, msg->bytes, got);
        done += got;
        if (got < chunk) break;
    }
    return done;
}
int _read(int fd, char *out, int n) {
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    int count = pread(fd, out, n, d->offset);
    if (count > 0)
        d->offset += count;
    return count;
}
ssize_t pwrite(int fd, const void *src, size_t n, off_t offset) {
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    if (offset < 0 || offset > INT_MAX || n > INT_MAX - (size_t)offset)
        return error(EFBIG);
    size_t done = 0;
    while (done < n) {
        int chunk = n - done > FILE_CAP ? FILE_CAP : (int)(n - done);
        msg->number = offset + done;
        msg->length = chunk;
        memcpy(msg->bytes, (const char *)src + done, chunk);
        if (call(LX_PWRITE, d->path) < 0) return done ? (ssize_t)done : -1;
        done += chunk;
    }
    return done;
}
int _write(int fd, const char *s, int n) {
    if (fd == 1 || fd == 2) {
        for (int i = 0; i < n; ++i) {
            if (s[i] == '\n')
                put_byte('\r');
            put_byte(s[i]);
        }
        return n;
    }
    Descriptor *d = descriptor(fd);
    if (!d)
        return -1;
    if (d->flags & O_APPEND && _lseek(fd, 0, SEEK_END) < 0)
        return -1;
    int written = pwrite(fd, s, n, d->offset);
    if (written > 0)
        d->offset += written;
    return written;
}
DIR *opendir(const char *p) {
    struct stat st;
    if (_stat(p, &st) < 0)
        return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    DIR *dir = calloc(1, sizeof(*dir));
    if (dir && canonical(dir->path, p) < 0) {
        free(dir);
        return NULL;
    }
    return dir;
}
struct dirent *readdir(DIR *dir) {
    msg->number = dir->next;
    int rc = call(LX_LIST, dir->path);
    if (rc != 0) {
        if (rc == 1)
            errno = 0;
        return NULL;
    }
    dir->next++;
    strcpy(dir->entry.d_name, msg->other);
    return &dir->entry;
}
int closedir(DIR *dir) {
    free(dir);
    return 0;
}
int _gettimeofday(struct timeval *t, void *tz) {
    (void)tz;
    t->tv_sec = rpc(LX_TIME);
    t->tv_usec = 0;
    return 0;
}
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
int getpwuid_r(uid_t id, struct passwd *p, char *b, size_t n, struct passwd **out) {
    (void)id;
    (void)p;
    (void)b;
    (void)n;
    *out = NULL;
    return 0;
}
long sysconf(int name) {
    if (name == _SC_PAGESIZE)
        return 4096;
    errno = EINVAL;
    return -1;
}
// Used only for collision-retried temporary filenames inside this private transaction.
uint64_t git_rand_next(void) {
    static uint64_t n;
    return ++n * UINT64_C(11400714819323198485);
}
int git_remote_create(git_remote **r, git_repository *repo, const char *n, const char *url) {
    (void)r;
    (void)repo;
    (void)n;
    (void)url;
    return -1;
}
void git_remote_free(git_remote *r) { (void)r; }
