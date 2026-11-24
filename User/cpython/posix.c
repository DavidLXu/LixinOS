// Userspace adapter for the musl calls made by the statically linked interpreter.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <signal.h>
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
#include <sys/un.h>
#include <sys/select.h>
#include <poll.h>
#include "../file-time.h"
#include "../tcc/lixin.h"
#include "../../Kernel/native/ipc.h"

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
    int fd_flags, local_bound;
    socklen_t local_length, peer_length;
    struct sockaddr_un local_address, local_peer;
    struct sockaddr_in peer;
};
static struct handle files[32];

static struct handle *handle(long fd) {
    return fd >= 3 && fd < 35 && files[fd - 3].path[0] ? &files[fd - 3] : NULL;
}
enum { TCP = 1, PIPE_FD, NULL_FD, LOCAL_FD, SHARED_FD };
enum { LOCAL_PAIR = 1, LOCAL_BIND, LOCAL_LISTEN, LOCAL_CONNECT, LOCAL_ACCEPT,
       LOCAL_READ, LOCAL_WRITE, LOCAL_SHUTDOWN, LOCAL_PEER, LOCAL_SENDMSG, LOCAL_RECVMSG,
       LOCAL_HAS_RIGHTS };
enum { SHARED_CREATE, SHARED_DUP, SHARED_RESIZE, SHARED_SIZE, SHARED_MAP, SHARED_UNMAP,
       SHARED_READ, SHARED_WRITE, SHARED_SEEK };

static long ipc_call(unsigned service, unsigned command, int fd, unsigned argument) {
    return console_call(service, (unsigned long)command << 56 |
                                 (unsigned long)(fd & 255) << 32 | argument);
}
#define unix_call(command, fd, argument) ipc_call(58, command, fd, argument)
#define shared_call(command, fd, argument) ipc_call(59, command, fd, argument)

static int stream_fd_from(int kind, int raw, int flags, int start) {
    for (int i = start; i < 32; ++i) if (!files[i].path[0]) {
        struct handle *h = &files[i];
        h->kind = kind; h->fd = raw; h->flags = flags;
        strcpy(h->path, kind == TCP || kind == LOCAL_FD ? "socket" :
                        kind == SHARED_FD ? "memfd" : "pipe");
        return i + 3;
    }
    return -EMFILE;
}
static int stream_fd(int kind, int raw, int flags) {
    return stream_fd_from(kind, raw, flags, 0);
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
    if (h && h->kind == SHARED_FD) return events & (POLLIN | POLLOUT);
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
    if (!buf) return -EFAULT;
    int length = n > 4096 ? 4096 : n;
    if (h->kind == SHARED_FD) {
        if (writing) memcpy(MAIL, buf, length);
        long result = shared_call(writing ? SHARED_WRITE : SHARED_READ, h->fd, length);
        if (!writing && result > 0) memcpy(buf, MAIL, result);
        return result;
    }
    for (;;) {
        int r;
        if (h->kind == LOCAL_FD) {
            if (writing) memcpy(MAIL, buf, length);
            r = unix_call(writing ? LOCAL_WRITE : LOCAL_READ, h->fd, length);
            if (r != -EAGAIN) {
                if (!writing && r > 0) memcpy(buf, MAIL, r);
                return r;
            }
        } else if (h->kind == TCP) {
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

static int local_fd(int raw, int flags) {
    int fd = stream_fd(LOCAL_FD, raw, O_RDWR | (flags & SOCK_NONBLOCK));
    if (fd >= 0) handle(fd)->fd_flags = flags & SOCK_CLOEXEC ? FD_CLOEXEC : 0;
    return fd;
}

static int shared_fd(int raw, int cloexec, int minimum) {
    if (raw < 0) return raw;
    int fd = stream_fd_from(SHARED_FD, raw, O_RDWR, minimum > 3 ? minimum - 3 : 0);
    if (fd >= 0) handle(fd)->fd_flags = cloexec ? FD_CLOEXEC : 0;
    else lx_close(raw);
    return fd;
}

static long duplicate_fd(struct handle *source, long minimum, int cloexec) {
    if (!source) return -EBADF;
    if (minimum < 0 || minimum >= 35) return -EINVAL;
    if (source->kind == SHARED_FD)
        return shared_fd(shared_call(SHARED_DUP, source->fd, 0), cloexec, minimum);
    if (source->fd < 0 || (source->kind != LOCAL_FD && source->kind != PIPE_FD))
        return -ENOTSUP;
    int fd = stream_fd_from(source->kind, source->fd, source->flags, minimum > 3 ? minimum - 3 : 0);
    if (fd >= 0) {
        *handle(fd) = *source;
        handle(fd)->fd_flags = cloexec ? FD_CLOEXEC : 0;
    }
    return fd;
}

static int raw_is_aliased(const struct handle *source) {
    if (source->kind != LOCAL_FD && source->kind != PIPE_FD) return 0;
    for (unsigned i = 0; i < 32; ++i)
        if (&files[i] != source && files[i].path[0] &&
            files[i].kind == source->kind && files[i].fd == source->fd) return 1;
    return 0;
}

static void nonblocking(struct handle *source, int enabled) {
    source->flags = (source->flags & ~O_NONBLOCK) | (enabled ? O_NONBLOCK : 0);
    if (source->fd < 0 || (source->kind != LOCAL_FD && source->kind != PIPE_FD)) return;
    for (unsigned i = 0; i < 32; ++i)
        if (files[i].path[0] && files[i].kind == source->kind && files[i].fd == source->fd)
            files[i].flags = (files[i].flags & ~O_NONBLOCK) | (enabled ? O_NONBLOCK : 0);
}

static int send_rights(const struct msghdr *message, unsigned raw[LX_IPC_RIGHTS]) {
    if (!message->msg_control) return message->msg_controllen ? -EFAULT : 0;
    const unsigned char *buffer = message->msg_control;
    size_t remaining = message->msg_controllen;
    unsigned count = 0;
    while (remaining) {
        struct cmsghdr header;
        if (remaining < CMSG_LEN(0)) return -EINVAL;
        memcpy(&header, buffer, sizeof(header));
        if (header.cmsg_len < CMSG_LEN(0) || header.cmsg_len > remaining) return -EINVAL;
        if (header.cmsg_level != SOL_SOCKET || header.cmsg_type != SCM_RIGHTS) return -EOPNOTSUPP;
        size_t bytes = header.cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) || bytes / sizeof(int) > LX_IPC_RIGHTS - count) return -EINVAL;
        for (unsigned i = 0; i < bytes / sizeof(int); ++i) {
            int fd;
            memcpy(&fd, buffer + CMSG_LEN(i * sizeof(fd)), sizeof(fd));
            struct handle *h = handle(fd);
            if (!h) return fd >= 0 && fd < 3 ? -EOPNOTSUPP : -EBADF;
            if (h->kind != SHARED_FD) return -EOPNOTSUPP;
            raw[count++] = h->fd;
        }
        size_t step = CMSG_ALIGN(header.cmsg_len);
        if (step >= remaining) break;
        remaining -= step;
        buffer += step;
    }
    return count;
}

static void receive_rights(struct msghdr *message, int flags, const unsigned *control) {
    unsigned count = 0;
    int fds[LX_IPC_RIGHTS];
    message->msg_flags = control[1] ? MSG_CTRUNC : 0;
    for (unsigned i = 0; i < control[0]; ++i) {
        int fd = shared_fd(control[2 + i], flags & MSG_CMSG_CLOEXEC, 0);
        if (fd >= 0) fds[count++] = fd;
        else message->msg_flags |= MSG_CTRUNC;
    }
    if (count) {
        size_t bytes = count * sizeof(int);
        size_t used = CMSG_SPACE(bytes);
        if (used > message->msg_controllen) used = message->msg_controllen;
        memset(message->msg_control, 0, used);
        const struct cmsghdr header = {
            .cmsg_len = CMSG_LEN(bytes), .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS
        };
        memcpy(message->msg_control, &header, sizeof(header));
        memcpy((char *)message->msg_control + CMSG_LEN(0), fds, bytes);
        message->msg_controllen = used;
    } else message->msg_controllen = 0;
    message->msg_namelen = 0;
}

static long local_message(struct handle *h, char *data, size_t size, unsigned count,
                          const unsigned *raw, struct msghdr *message, int writing, int flags) {
    if (h->fd < 0) return -ENOTCONN;
    if (writing && count && !size) return -EINVAL;
    const unsigned command = writing ? LOCAL_SENDMSG : LOCAL_RECVMSG;
    unsigned *control = (void *)(MAIL + LX_IPC_CONTROL);
    for (;;) {
        if (writing) {
            memcpy(MAIL, data, size);
            memcpy(control + 2, raw, count * sizeof(unsigned));
        }
        control[0] = count;
        long result = unix_call(command, h->fd, size);
        if (result != -EAGAIN) {
            if (!writing && result >= 0) {
                memcpy(data, MAIL, result);
                receive_rights(message, flags, control);
            }
            return result;
        }
        if (h->flags & O_NONBLOCK || flags & MSG_DONTWAIT) return -EAGAIN;
        lx_delay(1);
        if (lx_check_interrupt()) return -EINTR;
    }
}

static int local_address(const struct sockaddr_un *address, socklen_t length,
                         struct sockaddr_un *normalized) {
    const size_t prefix = offsetof(struct sockaddr_un, sun_path);
    if (!address) return -EFAULT;
    if (length <= prefix || length > prefix + sizeof(address->sun_path)) return -EINVAL;
    if (address->sun_family != AF_UNIX) return -EAFNOSUPPORT;
    *normalized = (struct sockaddr_un){.sun_family=AF_UNIX};
    size_t size = length - prefix;
    if (!address->sun_path[0]) {
        memcpy(normalized->sun_path, address->sun_path, size);
        return size;
    }
    char path[sizeof(address->sun_path) + 1], absolute[513];
    size = strnlen(address->sun_path, size);
    memcpy(path, address->sun_path, size);
    path[size] = 0;
    int error = resolve_at(AT_FDCWD, path, absolute);
    if (error) return error;
    size = strlen(absolute) + 1;
    if (size > sizeof(normalized->sun_path)) return -ENAMETOOLONG;
    memcpy(normalized->sun_path, absolute, size);
    return size;
}

static void local_name(struct sockaddr *address, socklen_t *length,
                       const struct sockaddr_un *value, socklen_t size) {
    if (address) {
        memcpy(address, value, *length < size ? *length : size);
        *length = size;
    }
}

static long local_vector(struct handle *h, const struct iovec *iov, long count,
                         int writing, int flags, struct msghdr *message) {
    if (count < 0 || count > IOV_MAX) return -EINVAL;
    if (count && !iov) return -EFAULT;
    char data[4096];
    unsigned raw[LX_IPC_RIGHTS], transfers = 0;
    if (message && unix_call(LOCAL_HAS_RIGHTS, 0, 0) != 1) {
        if (writing && message->msg_controllen) return -EOPNOTSUPP;
        if (!writing) {
            message->msg_controllen = message->msg_namelen = 0;
            message->msg_flags = 0;
        }
        message = NULL;
    }
    if (message && writing) {
        int found = send_rights(message, raw);
        if (found < 0) return found;
        transfers = found;
    } else if (message && message->msg_control && message->msg_controllen >= CMSG_LEN(0)) {
        size_t slots = (message->msg_controllen - CMSG_LEN(0)) / sizeof(int);
        unsigned free = 0;
        for (unsigned i = 0; i < 32; ++i) free += !files[i].path[0];
        transfers = slots > LX_IPC_RIGHTS ? LX_IPC_RIGHTS : slots;
        if (transfers > free) transfers = free;
    }
    size_t limit = message ? LX_IPC_CONTROL : sizeof(data);
    size_t size = 0;
    for (long i = 0; i < count && size < limit; ++i) {
        size_t n = iov[i].iov_len < limit - size ? iov[i].iov_len : limit - size;
        if (!n) continue;
        if (!iov[i].iov_base) return -EFAULT;
        if (writing) memcpy(data + size, iov[i].iov_base, n);
        size += n;
    }
    long result = message ? local_message(h, data, size, transfers, raw, message, writing, flags)
                          : stream_io(h, data, size, writing, flags);
    if (!writing && result > 0) {
        long copied = 0;
        for (long i = 0; i < count && copied < result; ++i) {
            size_t n = iov[i].iov_len < (size_t)(result - copied) ? iov[i].iov_len : (size_t)(result - copied);
            if (n) memcpy(iov[i].iov_base, data + copied, n);
            copied += n;
        }
    }
    return result;
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
    case SYS_kill:
        if (a <= 0 || b != SIGKILL) return -ENOTSUP;
        return console_call(55, a) < 0 ? -ESRCH : 0;
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
        if (b & O_CLOEXEC) handle(r)->fd_flags = handle(w)->fd_flags = FD_CLOEXEC;
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
        if (a == AF_UNIX) {
            if ((b & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM || c)
                return -EPROTONOSUPPORT;
            if (unix_call(0, 0, 0) != 1) return -EAFNOSUPPORT;
            return local_fd(-1, b);
        }
        if (a != AF_INET) return -EAFNOSUPPORT;
        if ((b & 15) != SOCK_STREAM || (c && c != IPPROTO_TCP)) return -EPROTONOSUPPORT;
        return stream_fd(TCP,-1,O_RDWR | (b & SOCK_NONBLOCK));
    case SYS_socketpair: {
        if (a != AF_UNIX || unix_call(0, 0, 0) != 1) return -EAFNOSUPPORT;
        if ((b & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM || c)
            return -EPROTONOSUPPORT;
        if (!d) return -EFAULT;
        int left = local_fd(-1, b);
        if (left < 0) return left;
        int right = local_fd(-1, b);
        long pair = right < 0 ? right : unix_call(LOCAL_PAIR, 0, 0);
        if (pair < 0) {
            memset(handle(left), 0, sizeof(*h));
            if (right >= 0) memset(handle(right), 0, sizeof(*h));
            return pair;
        }
        handle(left)->fd = pair >> 32;
        handle(right)->fd = (uint32_t)pair;
        ((int *)d)[0] = left;
        ((int *)d)[1] = right;
        return 0;
    }
    case SYS_memfd_create:
        if (!a) return -EFAULT;
        if (strnlen((const char *)a, 250) == 250 || (b & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))) return -EINVAL;
        if (unix_call(LOCAL_HAS_RIGHTS, 0, 0) != 1) return -ENOSYS;
        return shared_fd(shared_call(SHARED_CREATE, 0, 0), b & MFD_CLOEXEC, 0);
    case SYS_dup:
        return duplicate_fd(h, 0, 0);
    case SYS_bind:
    case SYS_connect: {
        if (h && h->kind == LOCAL_FD) {
            if (h->fd >= 0) return op == SYS_bind || h->local_bound ? -EINVAL : -EISCONN;
            struct sockaddr_un address;
            int length = local_address((const void *)b, c, &address);
            if (length < 0) return length;
            if (op == SYS_bind && address.sun_path[0] && lx_stat(address.sun_path) != LX_MISSING)
                return -EADDRINUSE;
            memcpy(MAIL, address.sun_path, length);
            int raw = unix_call(op == SYS_bind ? LOCAL_BIND : LOCAL_CONNECT, 0, length);
            if (raw < 0) return raw;
            h->fd = raw;
            if (op == SYS_bind) {
                h->local_bound = 1;
                h->local_address = address;
                h->local_length = offsetof(struct sockaddr_un, sun_path) + length;
            } else {
                h->local_peer = address;
                h->peer_length = offsetof(struct sockaddr_un, sun_path) + length;
            }
            return 0;
        }
        if (!h || h->kind != TCP) return -ENOTSOCK;
        if (op == SYS_bind) return -EOPNOTSUPP;
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
    case SYS_listen:
        if (!h || h->kind != LOCAL_FD) return -ENOTSOCK;
        if (!h->local_bound) return -EINVAL;
        return unix_call(LOCAL_LISTEN, h->fd, b < 0 ? 0 : b);
    case SYS_accept:
    case SYS_accept4: {
        if (!h || h->kind != LOCAL_FD) return -ENOTSOCK;
        if (!h->local_bound) return -EINVAL;
        if (b && !c) return -EFAULT;
        int flags = op == SYS_accept4 ? d : 0;
        if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
        int fd = local_fd(-1, flags);
        if (fd < 0) return fd;
        long raw;
        while ((raw = unix_call(LOCAL_ACCEPT, h->fd, 0)) == -EAGAIN && !(h->flags & O_NONBLOCK)) {
            lx_delay(1);
            if (lx_check_interrupt()) { raw = -EINTR; break; }
        }
        if (raw < 0) {
            memset(handle(fd), 0, sizeof(*h));
            return raw;
        }
        struct handle *accepted = handle(fd);
        accepted->fd = raw;
        accepted->local_address = h->local_address;
        accepted->local_length = h->local_length;
        const struct sockaddr_un peer = {.sun_family=AF_UNIX};
        local_name((void *)b, (void *)c, &peer, offsetof(struct sockaddr_un, sun_path));
        return fd;
    }
    case SYS_getsockname:
    case SYS_getpeername:
        if (h && h->kind == LOCAL_FD) {
            if (!b || !c) return -EFAULT;
            if (op == SYS_getpeername) {
                if (h->fd < 0 || unix_call(LOCAL_PEER, h->fd, 0) < 0) return -ENOTCONN;
                const struct sockaddr_un peer = {.sun_family=AF_UNIX};
                local_name((void *)b, (void *)c, h->peer_length ? &h->local_peer : &peer,
                           h->peer_length ? h->peer_length : offsetof(struct sockaddr_un, sun_path));
            } else {
                const struct sockaddr_un own = {.sun_family=AF_UNIX};
                local_name((void *)b, (void *)c, h->local_length ? &h->local_address : &own,
                           h->local_length ? h->local_length : offsetof(struct sockaddr_un, sun_path));
            }
            return 0;
        }
        if (op == SYS_getsockname) return -ENOTSUP;
        if (!h || h->kind != TCP || h->fd < 0) return -ENOTCONN;
        { socklen_t *len = (void *)c;
          memcpy((void *)b,&h->peer,*len < sizeof(h->peer) ? *len : sizeof(h->peer));
          *len = sizeof(h->peer); }
        return 0;
    case SYS_shutdown:
        if (!h || h->kind != LOCAL_FD) return -ENOTSOCK;
        if (b < SHUT_RD || b > SHUT_RDWR) return -EINVAL;
        return h->fd < 0 ? -ENOTCONN : unix_call(LOCAL_SHUTDOWN, h->fd, b);
    case SYS_setsockopt:
        return h && (h->kind == TCP || h->kind == LOCAL_FD) ? -ENOPROTOOPT : -ENOTSOCK;
    case SYS_getsockopt:
        if (!h || (h->kind != TCP && h->kind != LOCAL_FD)) return -ENOTSOCK;
        if (!d || !e) return -EFAULT;
        if (h->kind == LOCAL_FD && b == SOL_SOCKET && c == SO_PEERCRED) {
            if (*(socklen_t *)e < sizeof(struct ucred)) return -EINVAL;
            long pid = h->fd < 0 ? -ENOTCONN : unix_call(LOCAL_PEER, h->fd, 0);
            if (pid < 0) return pid;
            *(struct ucred *)d = (struct ucred){.pid=pid, .uid=getuid(), .gid=getgid()};
            *(socklen_t *)e = sizeof(struct ucred);
            return 0;
        }
        if (b != SOL_SOCKET || (c != SO_TYPE && c != SO_ERROR)) return -ENOPROTOOPT;
        if (*(socklen_t *)e < sizeof(int)) return -EINVAL;
        *(int *)d = c == SO_TYPE ? SOCK_STREAM : 0;
        *(socklen_t *)e = sizeof(int);
        return 0;
    case SYS_sendto:
    case SYS_recvfrom:
        if (!h || (h->kind != TCP && h->kind != LOCAL_FD)) return -ENOTSOCK;
        if (e || (d & ~(MSG_DONTWAIT | MSG_NOSIGNAL))) return -ENOTSUP;
        return stream_io(h,(void *)b,c,op == SYS_sendto,d);
    case SYS_sendmsg:
    case SYS_recvmsg: {
        if (!h || h->kind != LOCAL_FD) return -EOPNOTSUPP;
        struct msghdr *message = (void *)b;
        if (!message) return -EFAULT;
        int writing = op == SYS_sendmsg;
        if ((writing && message->msg_name) ||
            (c & ~(MSG_DONTWAIT | (writing ? MSG_NOSIGNAL : MSG_CMSG_CLOEXEC))))
            return -EOPNOTSUPP;
        return local_vector(h, message->msg_iov, message->msg_iovlen, writing, c, message);
    }
    case SYS_openat:
        return port_openat(a, path, c);
    case SYS_close:
        if (a >= 0 && a < 3) return lx_close(a) < 0 ? -EBADF : 0;
        if (!h)
            return -EBADF;
        error = 0;
        if (h->fd >= 0 && !raw_is_aliased(h) && lx_close(h->fd) < 0) error = -EIO;
        memset(h, 0, sizeof(*h));
        return error;
    case SYS_read:
    case SYS_write:
        return transfer(a, (void *)b, c, op == SYS_write);
    case SYS_readv:
    case SYS_writev: {
        if (h && h->kind == LOCAL_FD) return local_vector(h, (void *)b, c, op == SYS_writev, 0, NULL);
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
        if (op == SYS_ftruncate && h->kind == SHARED_FD) return shared_call(SHARED_RESIZE, h->fd, b);
        if (op == SYS_ftruncate && h->kind) return -EINVAL;
        int fd = op == SYS_truncate ? port_openat(AT_FDCWD, (const char *)a, O_WRONLY) : a;
        if (fd < 0) return fd;
        struct handle *file = handle(fd);
        long status = console_call(47, 1UL << 63 | (unsigned long)file->fd << 32 | (uint32_t)b);
        if (!status) file->size = b;
        if (op == SYS_truncate) lx_posix(SYS_close, fd,0,0,0,0,0);
        return status < 0 ? -EIO : 0;
    }
    case SYS_fallocate:
        if (!h || h->kind != SHARED_FD || b) return -ENOTSUP;
        if (c < 0 || d <= 0) return -EINVAL;
        if ((unsigned long)c > LX_SHARED_LIMIT || (unsigned long)d > LX_SHARED_LIMIT - c) return -EFBIG;
        {
            long size = shared_call(SHARED_SIZE, h->fd, 0);
            if (size < 0) return size;
            return size >= c + d ? 0 : shared_call(SHARED_RESIZE, h->fd, c + d);
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
        if (h && h->kind == SHARED_FD) {
            *(long *)MAIL = b;
            return shared_call(SHARED_SEEK, h->fd, c);
        }
        return seek_file(h, b, c);
    case SYS_getdents64:
        return port_getdents(h, (void *)b, c);
    case SYS_fchmod:
        if (!h) return -EBADF;
        return (b & 07777) == 0777 ? 0 : -ENOTSUP;
    case SYS_fstat:
        if (h && h->kind == NULL_FD) return stat_info("/dev/null", 0, (void *)b, 1);
        if (h && h->kind == SHARED_FD) {
            struct stat *st = (void *)b;
            long size = shared_call(SHARED_SIZE, h->fd, 0);
            if (size < 0) return size;
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFREG | 0600;
            st->st_blksize = 4096;
            st->st_size = size;
            st->st_blocks = (size + 511) / 512;
            return 0;
        }
        if (a >= 0 && a < 3)
            return stat_info("stdio", 0, (void *)b, 1);
        if (h && h->kind) { struct stat *st=(void *)b; memset(st,0,sizeof(*st)); st->st_mode=(h->kind == TCP || h->kind == LOCAL_FD) ? S_IFSOCK : S_IFIFO; return 0; }
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
        if (b == F_DUPFD || b == F_DUPFD_CLOEXEC)
            return duplicate_fd(h, c, b == F_DUPFD_CLOEXEC);
        if (b == F_SETFL && h && h->kind) {
            nonblocking(h, c & O_NONBLOCK);
            return 0;
        }
        if (b == F_GETFL)
            return h ? h->flags : a ? O_WRONLY : O_RDONLY;
        if (h && (h->kind == LOCAL_FD || h->kind == PIPE_FD || h->kind == SHARED_FD)) {
            if (b == F_GETFD) return h->fd_flags;
            if (b == F_SETFD) { h->fd_flags = c & FD_CLOEXEC; return 0; }
        }
        if (b == F_GETFD || b == F_SETFD)
            return 0;
        return -ENOTSUP;
    case SYS_ioctl:
        if (b == FIONBIO && h && h->kind) {
            nonblocking(h, *(int *)c);
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
        h = handle(e);
        if (h && h->kind == SHARED_FD) {
            if (d != MAP_SHARED) return -ENOTSUP;
            unsigned long *words = (void *)MAIL;
            words[0] = b; words[1] = f; words[2] = c;
            return shared_call(SHARED_MAP, h->fd, 0);
        }
        return lx_mmap(a, b, c, d, e);
    case SYS_munmap:
        if ((unsigned long)a >= LX_SHARED_BASE &&
            (unsigned long)a < LX_SHARED_BASE + LX_SHARED_LIMIT * LX_SHARED_MAPS) {
            unsigned long *words = (void *)MAIL;
            words[0] = a; words[1] = b;
            return shared_call(SHARED_UNMAP, 0, 0);
        }
        return lx_munmap(a, b);
    case SYS_brk:
        return lx_brk(a);
    case SYS_futex: return lx_futex((int *)a, b, c, (void *)d, (int *)e, f);
    case SYS_sched_yield: lx_schedule(); return 0;
    case SYS_rt_sigprocmask: return lx_sigmask(a, (void *)b, (void *)c);
    case SYS_prctl: return lx_prctl(a, b);
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
