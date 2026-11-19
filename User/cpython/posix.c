// Userspace adapter for the musl calls made by the statically linked interpreter.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "../console.h"
#include "runtime.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include "../file-time.h"
#include "../tcc/lixin.h"

long lx_mmap(long, unsigned long, long, long, long);
long lx_munmap(uintptr_t, unsigned long);
long lx_brk(uintptr_t);
extern int lx_save_buffer(const char *, const void *, unsigned);
int lx_getchar(void);
#define FILE_LIMIT (512L << 20) /* The mounted volume enforces its own limit. */
#define MAIL ((char *)0x443d7000)
struct handle {
    char path[513];
    long size, pos;
    int fd, flags;
    int kind;
    struct sockaddr_in peer;
};
static struct handle files[32];

static struct handle *handle(long fd) {
    return fd >= 3 && fd < 35 && files[fd - 3].path[0] ? &files[fd - 3] : NULL;
}
enum { TCP = 1, PIPE_FD, NULL_FD };
static int stream_fd(int kind, int raw, int flags) {
    for (int i = 0; i < 32; ++i) if (!files[i].path[0]) {
        struct handle *h = &files[i];
        h->kind = kind; h->fd = raw; h->flags = flags;
        strcpy(h->path, kind == TCP ? "socket" : "pipe");
        return i + 3;
    }
    return -EMFILE;
}
int lx_lookup_ipv4(const char *name, unsigned char *address) {
    if (strlen(name) > 253) return -1;
    strcpy(MAIL, name);
    long result = console_call(48, 0);
    if (result < 0) return -1;
    uint32_t ip = htonl((uint32_t)result);
    memcpy(address, &ip, 4);
    return 0;
}
static int ready(long fd, int events) {
    if (fd < 0) return 0;
    struct handle *h = handle(fd);
    if (!h && fd >= 3) return POLLNVAL;
    if (h && !h->kind) return events & (POLLIN | POLLOUT);
    if (h && h->kind == NULL_FD) return events & (POLLIN | POLLOUT);
    if (h && h->fd < 0) return POLLHUP;
    int interests = ((events & POLLIN) ? 1 : 0) | ((events & POLLOUT) ? 2 : 0);
    if (!interests) return 0;
    int r = lx_poll(h ? h->fd : fd, interests, 0);
    return r < 0 ? POLLERR : ((r & 1) ? POLLIN : 0) | ((r & 2) ? POLLOUT : 0);
}
static long wait_fds(struct pollfd *fds, unsigned n, const struct timespec *ts) {
    if (ts && (ts->tv_sec < 0 || (unsigned long)ts->tv_nsec >= 1000000000)) return -EINVAL;
    uint64_t end = ts ? console_call(38, 1) + ts->tv_sec*1000000ULL + ts->tv_nsec/1000 : UINT64_MAX;
    for (;;) {
        int count = 0;
        for (unsigned i = 0; i < n; ++i) {
            fds[i].revents = ready(fds[i].fd, fds[i].events);
            count += fds[i].revents != 0;
        }
        if (count || (uint64_t)console_call(38, 1) >= end) return count;
        lx_delay(1);
        if (lx_check_interrupt()) return -EINTR;
    }
}
static long stream_io(struct handle *h, void *buf, size_t n, int writing, int flags) {
    if (h->fd < 0) return -ENOTCONN;
    if (!n) return 0;
    int length = n > 4096 ? 4096 : n;
    for (;;) {
        int r;
        if (h->kind == TCP) {
            r = writing ? lx_tcp_write(h->fd, buf, length) : lx_tcp_read(h->fd, buf, length);
            if (r != LX_TCP_AGAIN) return r < 0 ? -ECONNRESET : r;
        } else {
            if ((h->flags & O_ACCMODE) != (writing ? O_WRONLY : O_RDONLY)) return -EBADF;
            if (lx_poll(h->fd, writing ? 2 : 1, 0) > 0) {
                r = writing ? lx_write(h->fd, buf, length) : lx_read(h->fd, buf, length);
                return r < 0 ? -EPIPE : r;
            }
        }
        if (h->flags & O_NONBLOCK || flags & MSG_DONTWAIT) return -EAGAIN;
        lx_delay(1);
        if (lx_check_interrupt()) return -EINTR;
    }
}
int lx_child_descriptor(int fd, int standard) {
    if (fd == -1 || fd == standard) return standard;
    if (standard == 2 && fd == 1) return 1;
    struct handle *h = handle(fd);
    if (h && h->kind == NULL_FD) return 255;
    return h && h->kind == PIPE_FD ? h->fd : -1;
}
static int children[64];
int lx_record_child(int pid) {
    for (unsigned i=0; i<64; i++) if (!children[i]) {children[i]=pid; return 0;}
    return -EAGAIN;
}
static long wait_child(int pid, int *status, int flags) {
    if (flags & ~1) return -ENOTSUP;
    for (;;) {
        int found=0;
        for (unsigned i=0; i<64; i++) if (children[i] && (pid == -1 || pid == children[i])) {
            found=1;
            int r = console_call(51, 1UL << 32 | (unsigned)children[i]);
            if (r == -2) continue;
            int child=children[i];
            children[i]=0;
            if (r < 0) return -ECHILD;
            if (status) *status = (r & 255) << 8;
            return child;
        }
        if (!found) return -ECHILD;
        if (flags & 1) return 0;
        lx_delay(2);
        if (lx_check_interrupt()) return -EINTR;
    }
}
static int resolve_at(long parent, const char *path, char absolute[513]) {
    size_t len = strnlen(path, 513);
    if (!len) return -ENOENT;
    if (len > 512) return -ENAMETOOLONG;
    if (parent != AT_FDCWD && *path != '/') {
        struct handle *dir = handle(parent);
        if (!dir) return -EBADF;
        if (dir->size != LX_DIR) return -ENOTDIR;
        size_t base = strlen(dir->path);
        if (base + 1 + len > 512) return -ENAMETOOLONG;
        memcpy(MAIL, dir->path, base);
        MAIL[base] = '/';
        memcpy(MAIL + base + 1, path, len + 1);
    } else strcpy(MAIL, path);
    if (console_call(23, 0) < 0) return -ENOENT;
    strcpy(absolute, MAIL);
    return 0;
}
static long port_openat(long parent, const char *path, int flags) {
    char absolute[513];
    if (!strcmp(path, "/dev/null")) {
        if (flags & O_DIRECTORY) return -ENOTDIR;
        return stream_fd(NULL_FD, -1, flags);
    }
    int error = resolve_at(parent, path, absolute);
    if (error) return error;
    if (!strcmp(absolute, "/dev/null")) {
        if (flags & O_DIRECTORY) return -ENOTDIR;
        return stream_fd(NULL_FD, -1, flags);
    }
    long size = lx_stat(absolute);
    if (size < 0 && size != LX_DIR && !(flags & O_CREAT))
        return -ENOENT;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) && (size >= 0 || size == LX_DIR))
        return -EEXIST;
    if ((flags & O_DIRECTORY) && size != LX_DIR)
        return -ENOTDIR;
    int mode = flags & O_ACCMODE;
    if (size == LX_DIR && mode != O_RDONLY)
        return -EISDIR;
    for (int i = 0; i < 32; ++i)
        if (!files[i].path[0]) {
            struct handle *h = &files[i];
            int fd = -1;
            if (size != LX_DIR) {
                if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
                    strcpy(MAIL, absolute);
                    if (console_call(31, 0) < 0)
                        return lx_stat(absolute) >= 0 ? -EEXIST : -EIO;
                }
                if (mode == O_RDONLY) fd = lx_open(absolute);
                else {
                    strcpy(MAIL, absolute);
                    fd = console_call(40, flags & O_TRUNC ? 0 : flags & O_APPEND ? 1 : 2);
                }
                if (fd < 0) return mode == O_RDONLY ? -ENOENT : -EIO;
                if (size < 0 || (flags & O_TRUNC))
                    size = 0;
            }
            if ((flags & O_APPEND) && size > 0 &&
                console_call(47, (unsigned long)fd << 32 | (uint32_t)size) < 0) {
                lx_close(fd);
                return -EIO;
            }
            h->size = size;
            h->pos = (flags & O_APPEND) ? size : 0;
            h->fd = fd;
            h->flags = flags;
            strcpy(h->path, absolute);
            return i + 3;
        }
    return -EMFILE;
}
static long stat_info(const char *name, long size, struct stat *st, int character) {
    if (size < 0 && size != LX_DIR)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->st_mode = (character ? S_IFCHR : size == LX_DIR ? S_IFDIR : S_IFREG) | 0777;
    uint32_t hash = 2166136261U;
    for (const char *p = name; *p; p++)
        hash = (hash ^ (unsigned char)*p) * 16777619U;
    st->st_ino = hash ? hash : 1;
    st->st_dev = 1;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_size = size < 0 ? 0 : size;
    st->st_blocks = (st->st_size + 511) / 512;
    if (!character && (lx_stat(name) >= 0 || size == LX_DIR)) {
        st->st_mtime = lx_file_time(1);
        st->st_ctime = lx_file_time(0);
    }
    return 0;
}
static int terminal(long fd) {
    if (fd == 0)
        return console_call(24, 2) == 1;
    return fd == 2 || (fd == 1 && lx_stdout_is_terminal());
}
static long console_input(char *out, size_t n) {
    static char line[4096];
    static size_t used, sent;
    if (!n)
        return 0;
    if (used == sent) {
        used = sent = 0;
        while (used < sizeof(line)) {
            int c = lx_input_byte();
            if (c == 3) { used = sent = 0; lx_signal_interrupt(); return -EINTR; }
            if ((unsigned)c > 255 || c == 4)
                break;
            if (c == 8 || c == 127) {
                if (used) {
                    used--;
                    lx_write(2, "\b \b", 3);
                }
                continue;
            }
            char ch = c == '\r' ? '\n' : c;
            line[used++] = ch;
            lx_write(2, &ch, 1);
            if (ch == '\n')
                break;
        }
    }
    if (n > used - sent)
        n = used - sent;
    memcpy(out, line + sent, n);
    sent += n;
    return n;
}
static long transfer(long fd, void *buf, size_t n, int write) {
    struct handle *h = handle(fd);
    if (fd < 0 || (!h && fd >= 3))
        return -EBADF;
    if (h && h->kind == NULL_FD) {
        int mode = h->flags & O_ACCMODE;
        if ((write && mode == O_RDONLY) || (!write && mode == O_WRONLY)) return -EBADF;
        return write ? (long)n : 0;
    }
    if (h && h->kind) return stream_io(h,buf,n,write,0);
    if (h && h->size == LX_DIR)
        return -EISDIR;
    if (!write && fd == 0 && terminal(0))
        return console_input(buf, n);
    if (h && ((write && (h->flags & O_ACCMODE) == O_RDONLY) ||
              (!write && (h->flags & O_ACCMODE) == O_WRONLY))) return -EBADF;
    int raw = h ? h->fd : fd;
    int count = n > 4096 ? 4096 : (int)n;
    long done = write ? lx_write(raw, buf, count) : lx_read(raw, buf, count);
    if (done < 0)
        return -EBADF;
    if (h) {
        if (write && (h->flags & O_APPEND)) h->pos = h->size = lx_stat(h->path);
        else h->pos += done;
        if (write && h->pos > h->size)
            h->size = h->pos;
    }
    return done;
}
static long seek_file(struct handle *h, long off, int whence) {
    if (!h || h->kind || h->size < 0)
        return -ESPIPE;
    if ((h->flags & O_ACCMODE) != O_RDONLY) h->size = lx_stat(h->path);
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? h->pos : h->size;
    long pos;
    if (whence < 0 || whence > 2 || __builtin_add_overflow(base, off, &pos) || pos < 0)
        return -EINVAL;
    if (pos > UINT32_MAX) return -EFBIG;
    if (console_call(47, (unsigned long)h->fd << 32 | (uint32_t)pos) < 0) return -EIO;
    h->pos = pos;
    return pos;
}
// Fast path for a complete copy into an empty file. The native save ABI
// publishes all bytes in one snapshot. Other copy shapes use libc's fallback.
static long copy_whole_file(long infd, long outfd, unsigned long length) {
    struct handle *in = handle(infd), *out = handle(outfd);
    if (!in || !out) return -EBADF;
    if (in->kind || out->kind || in->size < 0 || out->size < 0) return -EXDEV;
    if ((in->flags & O_ACCMODE) != O_RDONLY || (out->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (in->pos == in->size || !length) return 0;
    if (in->pos || out->pos || out->size || (unsigned long)in->size > length) return -EXDEV;
    unsigned char *data = malloc(in->size);
    if (!data) return -EXDEV;
    long done = 0;
    while (done < in->size) {
        int n = lx_read(in->fd, data + done, in->size - done);
        if (n <= 0) break;
        done += n;
    }
    long result = -EIO;
    if (done == in->size && lx_save_buffer(out->path, data, done) == 0) {
        in->pos = done;
        result = seek_file(out, done, SEEK_SET);
    } else {
        seek_file(in, 0, SEEK_SET);
    }
    free(data);
    return result;
}
static long port_getdents(struct handle *h, char *out, size_t cap) {
    if (!h || h->size != LX_DIR)
        return -ENOTDIR;
    size_t total = 0;
    while (total < cap) {
        char name[513];
        long size = lx_dir(h->path, h->pos, name);
        if (size == LX_END)
            break;
        if (size < LX_DIR)
            return -EIO;
        size_t n = strlen(name) + 1;
        size_t bytes = (offsetof(struct dirent, d_name) + n + 7) & ~(size_t)7;
        if (bytes > cap - total)
            return total ? (long)total : -EINVAL;
        struct dirent *d = (void *)(out + total);
        d->d_ino = ++h->pos;
        d->d_off = h->pos;
        d->d_reclen = bytes;
        d->d_type = size == LX_DIR ? DT_DIR : DT_REG;
        memcpy(out + total + offsetof(struct dirent, d_name), name, n);
        total += bytes;
    }
    return total;
}
long lx_posix(long op, long a, long b, long c, long d, long e, long f) {
    struct handle *h = handle(a);
    const char *path = (void *)b;
    char absolute[513], target[513];
    int error;
    switch (op) {
    case SYS_prlimit64: {
        if (a != 0 || b != RLIMIT_NOFILE || c) return -ENOTSUP;
        if (!d) return 0;
        long limit = console_call(42, 6);
        // Older kernels provide 16 file handles in addition to standard I/O.
        if (limit < 3) limit = 19;
        long local_limit = 3 + sizeof(files) / sizeof(files[0]);
        if (limit > local_limit) limit = local_limit;
        struct rlimit *info = (void *)d;
        info->rlim_cur = info->rlim_max = limit;
        return 0;
    }
    case SYS_exit:
        lx_thread_exit(NULL, 0);
    case SYS_exit_group:
        finish(a);
    case SYS_wait4: return wait_child(a,(int *)b,c);
    case SYS_pipe2: {
        if (b & ~(O_CLOEXEC|O_NONBLOCK)) return -EINVAL;
        long pair = console_call(49,0);
        if (pair < 0) return -EMFILE;
        int raw[2] = {pair >> 32, (uint32_t)pair};
        int r = stream_fd(PIPE_FD,raw[0],O_RDONLY|(b & O_NONBLOCK));
        int w = r < 0 ? -1 : stream_fd(PIPE_FD,raw[1],O_WRONLY|(b & O_NONBLOCK));
        if (w < 0) {
            if (r >= 0) memset(handle(r),0,sizeof(struct handle));
            lx_close(raw[0]); lx_close(raw[1]); return -EMFILE;
        }
        ((int *)a)[0] = r; ((int *)a)[1] = w;
        return 0;
    }
    case SYS_ppoll:
        return wait_fds((void *)a, b, (void *)c);
    case SYS_pselect6: {
        if (a < 0 || a > 35) return -EINVAL;
        fd_set *readers = (void *)b, *writers = (void *)c, *errors = (void *)d;
        struct pollfd list[35]; unsigned n = 0;
        for (int i = 0; i < a; ++i) {
            int mask = (readers && FD_ISSET(i,readers) ? POLLIN : 0) |
                       (writers && FD_ISSET(i,writers) ? POLLOUT : 0);
            if (mask) list[n++] = (struct pollfd){i,mask,0};
        }
        long r = wait_fds(list,n,(void *)e);
        if (r < 0) return r;
        if (readers) FD_ZERO(readers);
        if (writers) FD_ZERO(writers);
        if (errors) FD_ZERO(errors);
        r = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (list[i].revents & POLLNVAL) return -EBADF;
            if (readers && (list[i].revents & POLLIN)) {FD_SET(list[i].fd,readers); r++;}
            if (writers && (list[i].revents & POLLOUT)) {FD_SET(list[i].fd,writers); r++;}
        }
        return r;
    }
    case SYS_socket:
        if (a != AF_INET) return -EAFNOSUPPORT;
        if ((b & 15) != SOCK_STREAM || (c && c != IPPROTO_TCP)) return -EPROTONOSUPPORT;
        return stream_fd(TCP,-1,O_RDWR | (b & SOCK_NONBLOCK));
    case SYS_connect: {
        if (!h || h->kind != TCP) return -ENOTSOCK;
        if (h->fd >= 0) return -EISCONN;
        const struct sockaddr_in *ip = (void *)b;
        if (c < sizeof(*ip) || ip->sin_family != AF_INET) return -EAFNOSUPPORT;
        char name[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET,&ip->sin_addr,name,sizeof(name))) return -EINVAL;
        h->fd = lx_tcp_connect(name,ntohs(ip->sin_port));
        if (h->fd < 0) return -ECONNREFUSED;
        h->peer = *ip;
        return 0;
    }
    case SYS_getpeername:
        if (!h || h->kind != TCP || h->fd < 0) return -ENOTCONN;
        { socklen_t *len = (void *)c;
          memcpy((void *)b,&h->peer,*len < sizeof(h->peer) ? *len : sizeof(h->peer));
          *len = sizeof(h->peer); }
        return 0;
    case SYS_setsockopt: return h && h->kind == TCP ? -ENOPROTOOPT : -ENOTSOCK;
    case SYS_getsockopt:
        if (!h || h->kind != TCP) return -ENOTSOCK;
        if (b != SOL_SOCKET || (c != SO_TYPE && c != SO_ERROR)) return -ENOPROTOOPT;
        if (*(socklen_t *)e < sizeof(int)) return -EINVAL;
        *(int *)d = c == SO_TYPE ? SOCK_STREAM : 0;
        *(socklen_t *)e = sizeof(int);
        return 0;
    case SYS_sendto:
    case SYS_recvfrom:
        if (!h || h->kind != TCP) return -ENOTSOCK;
        if (e || (d & ~(MSG_DONTWAIT | MSG_NOSIGNAL))) return -ENOTSUP;
        return stream_io(h,(void *)b,c,op == SYS_sendto,d);
    case SYS_openat:
        return port_openat(a, path, c);
    case SYS_close:
        if (a >= 0 && a < 3) return lx_close(a) < 0 ? -EBADF : 0;
        if (!h)
            return -EBADF;
        error = 0;
        if (h->fd >= 0 && lx_close(h->fd) < 0) error = -EIO;
        memset(h, 0, sizeof(*h));
        return error;
    case SYS_read:
    case SYS_write:
        return transfer(a, (void *)b, c, op == SYS_write);
    case SYS_readv:
    case SYS_writev: {
        struct iovec *vec = (void *)b;
        long done = 0;
        for (long i = 0; i < c; i++) {
            if (!vec[i].iov_len)
                continue;
            long n = transfer(a, vec[i].iov_base, vec[i].iov_len, op == SYS_writev);
            if (n < 0)
                return done ? done : n;
            done += n;
            if ((size_t)n < vec[i].iov_len || (op == SYS_readv && a == 0))
                break;
        }
        return done;
    }
    case SYS_truncate:
    case SYS_ftruncate: {
        if (b < 0 || b > FILE_LIMIT) return b < 0 ? -EINVAL : -EFBIG;
        if (op == SYS_ftruncate && (!h || (h->flags & O_ACCMODE) == O_RDONLY)) return -EBADF;
        if (op == SYS_ftruncate && h->kind) return -EINVAL;
        int fd = op == SYS_truncate ? port_openat(AT_FDCWD, (const char *)a, O_WRONLY) : a;
        if (fd < 0) return fd;
        struct handle *file = handle(fd);
        long status = console_call(47, 1UL << 63 | (unsigned long)file->fd << 32 | (uint32_t)b);
        if (!status) file->size = b;
        if (op == SYS_truncate) lx_posix(SYS_close, fd,0,0,0,0,0);
        return status < 0 ? -EIO : 0;
    }
    case SYS_copy_file_range:
        if (f) return -EINVAL;
        if (b || d) return -EXDEV;
        return copy_whole_file(a, c, e);
    case SYS_pread64:
    case SYS_pwrite64: {
        if (!h || h->kind || h->size < 0) return -ESPIPE;
        long saved = h->pos;
        long pos = seek_file(h, d, SEEK_SET);
        if (pos < 0) return pos;
        long count = transfer(a, (void *)b, c, op == SYS_pwrite64);
        long restored = seek_file(h, saved, SEEK_SET);
        return restored < 0 ? restored : count;
    }
    case SYS_lseek:
        return seek_file(h, b, c);
    case SYS_getdents64:
        return port_getdents(h, (void *)b, c);
    case SYS_fchmod:
        if (!h) return -EBADF;
        return (b & 07777) == 0777 ? 0 : -ENOTSUP;
    case SYS_fstat:
        if (h && h->kind == NULL_FD) return stat_info("/dev/null", 0, (void *)b, 1);
        if (a >= 0 && a < 3)
            return stat_info("stdio", 0, (void *)b, 1);
        if (h && h->kind) { struct stat *st=(void *)b; memset(st,0,sizeof(*st)); st->st_mode=h->kind == TCP ? S_IFSOCK : S_IFIFO; return 0; }
        return h ? stat_info(h->path, h->size, (void *)b, 0) : -EBADF;
    case SYS_newfstatat:
        error = resolve_at(a, path, absolute);
        return error ? error : stat_info(absolute, lx_stat(absolute), (void *)c, 0);
    case SYS_faccessat:
    case SYS_faccessat2:
    case SYS_fchmodat:
        error = resolve_at(a, path, absolute);
        return error ? error : lx_stat(absolute) == LX_MISSING ? -ENOENT : 0;
    case SYS_renameat:
    case SYS_renameat2:
        if (op == SYS_renameat2 && e) return -ENOTSUP;
        error = resolve_at(a, path, absolute);
        if (!error) error = resolve_at(c, (void *)d, target);
        if (error) return error;
        if (lx_stat(absolute) == LX_MISSING) return -ENOENT;
        if (!strcmp(absolute, target)) return 0;
        for (;;) {
            if (console_call(45, 0) < 0) return -EBUSY;
            strcpy(MAIL, absolute);
            strcpy(MAIL + 513, target);
            if (console_call(45, 5) >= 0 && console_call(45, 6) >= 0) return 0;
            // A concurrent writer may commit between the three batch operations.
            int retry = !strcmp(MAIL, "filesystem changed during transaction; retry");
            console_call(45, 7);
            if (!retry) return -EIO;
        }
    case SYS_readlinkat: {
        if (!strcmp(path, "/proc/self/exe")) {
            const char *exe = (const char *)getauxval(AT_EXECFN);
            if (!exe || !*exe) return -ENOENT;
            if (!d) return -EINVAL;
            size_t n = strnlen(exe, (size_t)d);
            memcpy((void *)c, exe, n);
            return n;
        }
        error = resolve_at(a, path, absolute);
        return error ? error : lx_stat(absolute) == LX_MISSING ? -ENOENT : -EINVAL;
    }
    case SYS_getcwd: {
        long length = console_call(42, 0);
        if (length < 0 || length > 512)
            return -EIO;
        MAIL[length] = 0;
        size_t n = (size_t)length + 1;
        if (n > (size_t)b)
            return -ERANGE;
        memcpy((void *)a, MAIL, n);
        return n;
    }
    case SYS_chdir:
    case SYS_mkdirat:
    case SYS_unlinkat: {
        if (op == SYS_chdir)
            path = (void *)a;
        error = resolve_at(op == SYS_chdir ? AT_FDCWD : a, path, absolute);
        if (error) return error;
        long size = lx_stat(absolute);
        if (op == SYS_mkdirat && size != LX_MISSING) return -EEXIST;
        if (op == SYS_unlinkat && size == LX_MISSING) return -ENOENT;
        strcpy(MAIL, absolute);
        return console_call(42, op == SYS_chdir      ? 1
                                : op == SYS_mkdirat  ? 2
                                : (c & AT_REMOVEDIR) ? 4
                                                     : 3);
    }
    case SYS_umask: {
        static unsigned mask = 022;
        unsigned previous = mask;
        mask = (unsigned)a & 0777;
        return previous;
    }
    case SYS_fsync:
    case SYS_fdatasync:
        // Guest file writes commit a disk snapshot before returning.
        return h ? 0 : -EBADF;
    case SYS_fcntl:
        if (!h && (a < 0 || a > 2))
            return -EBADF;
        if ((b == F_DUPFD || b == F_DUPFD_CLOEXEC) && h && h->size == LX_DIR && c == 0)
            return port_openat(AT_FDCWD, h->path, O_DIRECTORY);
        if (b == F_SETFL && h && h->kind) {
            h->flags = (h->flags & ~O_NONBLOCK) | (c & O_NONBLOCK);
            return 0;
        }
        if (b == F_GETFL)
            return h ? h->flags : a ? O_WRONLY : O_RDONLY;
        if (b == F_GETFD || b == F_SETFD)
            return 0;
        return -ENOTSUP;
    case SYS_ioctl:
        if (b == FIONBIO && h && h->kind) {
            if (*(int *)c) h->flags |= O_NONBLOCK; else h->flags &= ~O_NONBLOCK;
            return 0;
        }
        if (b == TIOCGWINSZ && terminal(a)) {
            struct winsize *w = (void *)c;
            unsigned long v = lx_geometry();
            *w = (struct winsize){.ws_row = (v >> 16) & 65535, .ws_col = v & 65535};
            return 0;
        }
        return -ENOTTY;
    case SYS_mmap:
        return lx_mmap(a, b, c, d, e);
    case SYS_munmap:
        return lx_munmap(a, b);
    case SYS_brk:
        return lx_brk(a);
    case SYS_futex: return lx_futex((int *)a, b, c, (void *)d);
    case SYS_sched_yield: lx_schedule(); return 0;
    case SYS_rt_sigprocmask: return lx_sigmask(a, (void *)b, (void *)c);
    case SYS_rt_sigaction: {
        static uint8_t actions[65][32];
        if (a <= 0 || a > 64 || d != 8) return -EINVAL;
        if (c) memcpy((void *)c, actions[a], 32);
        if (b) memcpy(actions[a], (void *)b, 32);
        return 0;
    }
    case SYS_gettid: return lx_thread_id();
    case SYS_set_tid_address: return lx_clear_tid((int *)a);
    case SYS_getpid: {
        long pid = console_call(54, 0);
        return pid >= 0 ? pid : lx_pid();
    }
    case SYS_getuid:
    case SYS_getgid:
    case SYS_geteuid:
    case SYS_getegid:
        return 0;
    case SYS_clock_gettime: {
        struct timespec *ts = (void *)b;
        if (a == CLOCK_REALTIME) {
            ts->tv_sec = console_call(36, 0);
            ts->tv_nsec = 0;
        } else if (a == CLOCK_MONOTONIC) {
            long us = console_call(38, 1);
            ts->tv_sec = us / 1000000;
            ts->tv_nsec = us % 1000000 * 1000;
        } else
            return -EINVAL;
        return 0;
    }
    case SYS_nanosleep:
    case SYS_clock_nanosleep: {
        const struct timespec *ts = (void *)(op == SYS_nanosleep ? a : c);
        if (ts->tv_sec < 0 || ts->tv_sec > LONG_MAX / 1000 - 1 || ts->tv_nsec < 0 ||
            ts->tv_nsec >= 1000000000L)
            return -EINVAL;
        long ms = ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
        if (op == SYS_clock_nanosleep) {
            if (a != CLOCK_MONOTONIC && a != CLOCK_REALTIME)
                return -EINVAL;
            if (b & ~TIMER_ABSTIME)
                return -EINVAL;
            if (b & TIMER_ABSTIME)
                ms -= a == CLOCK_MONOTONIC ? console_call(38, 0) : console_call(36, 0) * 1000;
        }
        while (ms > 0) {
            long interval = ms > 20 ? 20 : ms;
            lx_delay(interval);
            if (lx_check_interrupt()) return -EINTR;
            ms -= interval;
        }
        return 0;
    }
    case SYS_getrandom:
        return lx_random((void *)a, b) == 0 ? b : -EIO;
    case SYS_uname: {
        struct utsname *u = (void *)a;
        memset(u, 0, sizeof(*u));
        strcpy(u->sysname, "LixinOS");
        strcpy(u->machine, "aarch64");
        strcpy(u->nodename, "lixinos");
        strcpy(u->release, "0.1.0");
        return 0;
    }
    default:
        return -ENOSYS;
    }
}
