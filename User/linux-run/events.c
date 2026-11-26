// Level-triggered poll sets for Linux guests, over the LixinOS poll backend.
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct watch {
    int fd;
    u64 key;
    struct epoll_event ev;
    struct watch *next;
};
enum { COUNTER = 1, POLLSET, CHANNEL };
enum { CAPACITY = 16384 };
struct stream_control {
    struct stream_control *next;
    struct rights *rights;
    size_t offset;
};
struct queue {
    struct stream_control *controls;
    size_t count;
    unsigned char data[CAPACITY];
};
struct pair {
    struct queue input[2];
    int open[2], read_closed[2], write_closed[2];
    int packet, pipe, in_transit, passcred[2], nonblock[2];
};
struct monitor {
    unsigned refs, flags;
    u64 count;
    struct watch *watches;
};
struct event {
    int kind, flags;
    struct monitor *monitor;
    struct pair *pair;
    unsigned side;
};
struct guest_events {
    struct event slots[GUEST_FDS];
    u64 keys[GUEST_FDS];
    struct guest_process *owner;
    struct guest_events *next;
};
static struct guest_events *tables;
static u64 next_key;
static int event_epoch;
static void wake_events(void) {
    __atomic_add_fetch(&event_epoch, 1, __ATOMIC_RELEASE);
    thread_futex_wake(&event_epoch, INT_MAX);
}
static long wait_events(CPU *cpu, int snap, int has_native, long rem_ms) {
    struct timespec ts;
    const struct timespec *timeout = NULL;
    if (has_native) {
        long wait_ms = (rem_ms >= 0 && rem_ms < 1) ? rem_ms : 1;
        ts.tv_sec = wait_ms / 1000;
        ts.tv_nsec = (wait_ms % 1000) * 1000000L;
        timeout = &ts;
    } else if (rem_ms >= 0) {
        ts.tv_sec = rem_ms / 1000;
        ts.tv_nsec = (rem_ms % 1000) * 1000000L;
        timeout = &ts;
    }
    return thread_futex_wait(cpu, &event_epoch, snap, 0, timeout);
}
u64 event_key(unsigned fd) {
    u64 *key = &guest_process->event_table->keys[fd];
    if (!*key)
        *key = ++next_key;
    return *key;
}
void event_set_key(unsigned fd, u64 key) { guest_process->event_table->keys[fd] = key; }
static struct guest_events initial_events;
#define event_slots                                                                                \
    ((guest_process->event_table ? guest_process->event_table : &initial_events)->slots)

enum { MAX_RIGHTS = 32 };
struct rights {
    size_t count;
    struct held_fd *fds[MAX_RIGHTS];
};
struct packet_header {
    struct rights *rights;
    unsigned bytes;
    int pid;
    struct pid_namespace sender;
};

struct held_channel {
    struct event saved;
};
static void drop_monitor(struct monitor *m) {
    if (!m || --m->refs)
        return;
    while (m->watches) {
        struct watch *w = m->watches;
        m->watches = w->next;
        free(w);
    }
    free(m);
}

static void free_rights(struct rights *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->count; i++)
        drop_fd(r->fds[i]);
    free(r);
}
static void release_side(struct pair *p, unsigned side) {
    if (!p || --p->open[side])
        return;
    struct queue *q = &p->input[side];
    if (p->packet) {
        for (size_t off = 0; off < q->count;) {
            struct packet_header packet;
            memcpy(&packet, q->data + off, sizeof packet);
            free_rights(packet.rights);
            off += sizeof packet + packet.bytes;
        }
    }
    while (q->controls) {
        struct stream_control *c = q->controls;
        q->controls = c->next;
        free_rights(c->rights);
        free(c);
    }
    q->count = 0;
    wake_events();
    if (!p->open[side ^ 1])
        free(p);
}
long hold_channel(int fd, int transport, struct held_channel **out) {
    struct event *e = &event_slots[fd];
    // Keep references acyclic so closing the last endpoint can release all
    // undelivered rights without a process-wide Unix-socket garbage collector.
    struct pair *carrier = event_slots[transport].pair;
    if (e->pair && (e->pair == carrier || carrier->in_transit))
        return -ELOOP;
    struct held_channel *h = malloc(sizeof *h);
    if (!h)
        return -ENOMEM;
    h->saved = *e;
    if (e->monitor)
        ++e->monitor->refs;
    if (e->pair) {
        ++e->pair->in_transit;
        ++e->pair->open[e->side];
    }
    *out = h;
    return 0;
}
void drop_channel(struct held_channel *h) {
    if (!h)
        return;
    if (h->saved.pair) {
        --h->saved.pair->in_transit;
        release_side(h->saved.pair, h->saved.side);
    }
    drop_monitor(h->saved.monitor);
    free(h);
}
void receive_channel(const struct held_channel *h, int fd, int cloexec) {
    event_slots[fd] = h->saved;
    event_slots[fd].flags = cloexec ? O_CLOEXEC : 0;
    if (h->saved.monitor)
        ++h->saved.monitor->refs;
    if (h->saved.pair)
        ++h->saved.pair->open[h->saved.side];
}

static long parse_rights(int fd, const void *control, size_t bytes, struct rights **out) {
    struct rights *r = calloc(1, sizeof *r);
    if (!r)
        return -ENOMEM;
    *out = r;
    for (size_t off = 0; off < bytes;) {
        struct cmsghdr cm;
        size_t left = bytes - off;
        if (left < sizeof cm)
            return -EINVAL;
        memcpy(&cm, (const char *)control + off, sizeof cm);
        if (cm.cmsg_len < CMSG_LEN(0) || cm.cmsg_len > left)
            return -EINVAL;
        if (cm.cmsg_level != SOL_SOCKET || cm.cmsg_type != SCM_RIGHTS)
            return -ENOTSUP;
        size_t data = cm.cmsg_len - CMSG_LEN(0);
        if (data % sizeof(int) || data / sizeof(int) > MAX_RIGHTS - r->count)
            return -EINVAL;
        for (size_t pos = 0; pos < data; pos += sizeof(int)) {
            int src;
            memcpy(&src, (const char *)control + off + CMSG_LEN(0) + pos, sizeof src);
            long rc = hold_fd(src, fd, &r->fds[r->count]);
            if (rc)
                return rc;
            ++r->count;
        }
        off += CMSG_ALIGN(cm.cmsg_len);
    }
    return 0;
}
static void write_rights(struct msghdr *msg, void *control, size_t capacity, const struct rights *r,
                         int cloexec) {
    if (!r || !r->count)
        return;
    size_t room = capacity - msg->msg_controllen;
    size_t fit = room >= CMSG_LEN(0) ? (room - CMSG_LEN(0)) / sizeof(int) : 0;
    char *data = control ? (char *)control + msg->msg_controllen : NULL;
    size_t delivered = 0;
    while (delivered < fit && delivered < r->count) {
        int fd = receive_fd(r->fds[delivered], cloexec);
        if (fd < 0)
            break;
        memcpy(data + CMSG_LEN(0) + delivered * sizeof(int), &fd, sizeof fd);
        ++delivered;
    }
    if (delivered != r->count)
        msg->msg_flags |= MSG_CTRUNC;
    if (!delivered)
        return;
    struct cmsghdr cm = {.cmsg_level = SOL_SOCKET,
                         .cmsg_type = SCM_RIGHTS,
                         .cmsg_len = CMSG_LEN(delivered * sizeof(int))};
    memcpy(data, &cm, sizeof cm);
    size_t used = CMSG_ALIGN(cm.cmsg_len);
    if (used > room)
        used = room;
    memset(data + cm.cmsg_len, 0, used - cm.cmsg_len);
    msg->msg_controllen += used;
}

int event_fd(int fd) { return fd >= 0 && fd < GUEST_FDS ? event_slots[fd].kind : 0; }
// Registrations use the open description and the fd number supplied to epoll_ctl.
// A child can reuse that number without redirecting a parent's registration.
static int locate_key(u64 key, struct guest_process **owner) {
    for (struct guest_events *t = tables; t; t = t->next)
        for (int fd = 0; fd < GUEST_FDS; ++fd)
            if (t->keys[fd] == key) {
                *owner = t->owner;
                return fd;
            }
    return -1;
}
void forget_event_key(u64 key) {
    struct guest_process *owner;
    if (!key || locate_key(key, &owner) >= 0 || held_event_key(key))
        return;
    for (struct guest_events *t = tables; t; t = t->next)
        for (int fd = 0; fd < GUEST_FDS; ++fd) {
            struct event *e = &t->slots[fd];
            if (e->kind != POLLSET)
                continue;
            struct watch **link = &e->monitor->watches;
            while (*link) {
                struct watch *w = *link;
                if (w->key == key) {
                    *link = w->next;
                    free(w);
                } else
                    link = &w->next;
            }
        }
}
static void close_slot(struct guest_events *t, unsigned fd) {
    struct event *e = &t->slots[fd];
    u64 key = t->keys[fd];
    t->keys[fd] = 0;
    release_side(e->pair, e->side);
    drop_monitor(e->monitor);
    *e = (struct event){0};
    forget_event_key(key);
}
void event_closed(unsigned fd) { close_slot(guest_process->event_table, fd); }

int event_ready(int fd, short requested) {
    if (fd < 0)
        return 0;
    if (event_fd(fd) == CHANNEL) {
        struct event *e = &event_slots[fd];
        struct pair *p = e->pair;
        unsigned side = e->side;
        int bits = 0;
        int eof = p->read_closed[side] || !p->open[side ^ 1];
        if ((!p->pipe || !side) && (p->input[side].count || eof))
            bits |= POLLIN;
        if ((!p->pipe || side) && (p->input[side ^ 1].count < CAPACITY || p->write_closed[side]) &&
            p->open[side ^ 1])
            bits |= POLLOUT;
        if (!p->open[side ^ 1] || (p->read_closed[side] && p->write_closed[side]))
            bits |= POLLHUP;
        return bits & (requested | POLLHUP);
    }
    if (event_fd(fd) == COUNTER) {
        u64 count = event_slots[fd].monitor->count;
        return requested & ((count ? POLLIN : 0) | (count < UINT64_MAX - 1 ? POLLOUT : 0));
    }
    int raw = host_descriptor(fd);
    if (raw < 0)
        return POLLNVAL;
    struct pollfd item = {.fd = raw, .events = requested};
    return poll(&item, 1, 0) < 0 ? POLLERR : item.revents;
}
long channel_pair(CPU *cpu) {
    const u64 *a = cpu->x;
    int pipe = a[8] == SYS_pipe2;
    unsigned flags = pipe ? a[1] : a[1] & ~15u;
    unsigned kind = pipe ? SOCK_STREAM : a[1] & 15u;
    if (!pipe && (a[0] != AF_UNIX || a[2]))
        return -EAFNOSUPPORT;
    if (kind != SOCK_STREAM && kind != SOCK_SEQPACKET && kind != SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;
    int *dest = guest_ptr(pipe ? a[0] : a[3], 2 * sizeof(int), PROT_WRITE);
    if (!dest)
        return -EFAULT;
    struct pair *p = calloc(1, sizeof *p);
    if (!p)
        return -ENOMEM;
    p->open[0] = p->open[1] = 1;
    p->packet = kind != SOCK_STREAM;
    p->pipe = pipe;
    p->nonblock[0] = p->nonblock[1] = flags & O_NONBLOCK;
    int fds[2];
    for (int i = 0; i < 2; i++) {
        fds[i] = free_guest_fd();
        if (fds[i] < 0) {
            if (i)
                event_slots[fds[0]] = (struct event){0};
            free(p);
            return -EMFILE;
        }
        event_slots[fds[i]] = (struct event){.kind = CHANNEL, .flags = flags, .pair = p, .side = i};
    }
    memcpy(dest, fds, sizeof fds);
    return 0;
}
long channel_shutdown(int fd, u64 how) {
    if (event_fd(fd) != CHANNEL || event_slots[fd].pair->pipe)
        return -ENOTSOCK;
    if (how > SHUT_RDWR)
        return -EINVAL;
    struct pair *p = event_slots[fd].pair;
    unsigned side = event_slots[fd].side;
    if (how != SHUT_WR)
        p->read_closed[side] = p->write_closed[side ^ 1] = 1;
    if (how != SHUT_RD)
        p->write_closed[side] = p->read_closed[side ^ 1] = 1;
    wake_events();
    return 0;
}
// Store credentials when a datagram is queued. A later recvmsg may run in a
// different process and user namespace, even when the sending process has exited.
long channel_sockopt(CPU *cpu) {
    const u64 *a = cpu->x;
    struct event *e = &event_slots[a[0]];
    if (e->kind != CHANNEL || e->pair->pipe)
        return -ENOTSOCK;
    if (a[1] != SOL_SOCKET || a[2] != SO_PASSCRED || !e->pair->packet)
        return -ENOPROTOOPT;
    int *enabled = &e->pair->passcred[e->side];
    if (a[8] == SYS_setsockopt) {
        if (a[4] < sizeof(int))
            return -EINVAL;
        int value;
        void *in = guest_ptr(a[3], sizeof value, PROT_READ);
        if (!in)
            return -EFAULT;
        memcpy(&value, in, sizeof value);
        *enabled = !!value;
        return 0;
    }
    socklen_t *len = guest_ptr(a[4], sizeof *len, PROT_READ | PROT_WRITE);
    if (!len)
        return -EFAULT;
    size_t n = *len < sizeof(int) ? *len : sizeof(int);
    void *out = guest_ptr(a[3], n, PROT_WRITE);
    if (n && !out)
        return -EFAULT;
    if (n)
        memcpy(out, enabled, n);
    *len = n;
    return 0;
}

static long channel_transfer(int fd, struct iovec *iov, size_t count, size_t size, int writing,
                             int flags, struct msghdr *msg, void *control) {
    struct event *e = &event_slots[fd];
    struct pair *p = e->pair;
    unsigned side = e->side, peer = side ^ 1;
    if (p->pipe && writing != side)
        return -EBADF;
    if (!size && !p->packet)
        return 0;
    struct queue *q = &p->input[writing ? peer : side];
    size_t hdr = p->packet ? sizeof(struct packet_header) : 0;
    size_t room = CAPACITY - q->count;
    struct packet_header packet = {0};
    size_t bytes;
    if (writing) {
        if (!p->open[peer] || p->write_closed[side])
            return -EPIPE;
        if (hdr && size > CAPACITY - hdr)
            return -EMSGSIZE;
        if (!room || (hdr && room < size + hdr) || (p->pipe && size <= 4096 && size > room))
            return -EAGAIN;
        bytes = size < room - hdr ? size : room - hdr;
        if (msg && msg->msg_controllen) {
            if (p->pipe)
                return -ENOTSOCK;
            long rc = parse_rights(fd, control, msg->msg_controllen, &packet.rights);
            if (rc) {
                free_rights(packet.rights);
                return rc;
            }
            if (!hdr) {
                struct stream_control *c = malloc(sizeof *c);
                if (!c) {
                    free_rights(packet.rights);
                    return -ENOMEM;
                }
                *c = (struct stream_control){.offset = q->count, .rights = packet.rights};
                struct stream_control **tail = &q->controls;
                while (*tail)
                    tail = &(*tail)->next;
                *tail = c;
            }
        }
        packet.bytes = bytes;
        packet.sender = guest_process->pids;
        packet.pid = (p->passcred[side] || p->passcred[peer]) ? guest_process->pid : 0;
    } else {
        if (!q->count) {
            if (p->open[peer] && !p->read_closed[side])
                return -EAGAIN;
            if (msg) {
                msg->msg_flags = 0;
                msg->msg_controllen = 0;
            }
            return 0;
        }
        if (hdr)
            memcpy(&packet, q->data, hdr);
        else {
            packet.bytes = q->count;
            struct stream_control *c = q->controls;
            if (c && c->offset == 0) {
                packet.rights = c->rights;
                c = c->next;
            }
            if (c)
                packet.bytes = c->offset;
        }
        bytes = size < packet.bytes ? size : packet.bytes;
    }
    size_t offset = writing ? q->count : 0;
    if (writing && hdr)
        memcpy(q->data + offset, &packet, hdr);
    unsigned char *data = q->data + offset + hdr;
    size_t remaining = bytes;
    for (size_t i = 0; i < count && remaining; i++) {
        size_t n = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
        if (!n)
            continue;
        if (writing)
            memcpy(data, iov[i].iov_base, n);
        else
            memcpy(iov[i].iov_base, data, n);
        data += n;
        remaining -= n;
    }
    if (writing) {
        q->count += bytes + hdr;
        wake_events();
        return bytes;
    }
    if (msg) {
        size_t capacity = msg->msg_controllen;
        msg->msg_flags = hdr && bytes < packet.bytes ? MSG_TRUNC : 0;
        msg->msg_controllen = 0;
        if (hdr && p->passcred[side]) {
            struct ucred cred = {
                .pid = namespace_pid(&guest_process->pids, &packet.sender, packet.pid),
                .uid = packet.pid ? namespace_id(0) : 65534,
                .gid = packet.pid ? namespace_id(1) : 65534};
            struct cmsghdr cm = {.cmsg_level = SOL_SOCKET,
                                 .cmsg_type = SCM_CREDENTIALS,
                                 .cmsg_len = CMSG_LEN(sizeof cred)};
            if (capacity < cm.cmsg_len)
                msg->msg_flags |= MSG_CTRUNC;
            if (capacity >= CMSG_LEN(0)) {
                if (cm.cmsg_len > capacity)
                    cm.cmsg_len = capacity;
                size_t used = CMSG_SPACE(sizeof cred);
                if (used > capacity)
                    used = capacity;
                memset(control, 0, used);
                memcpy(control, &cm, sizeof cm);
                memcpy((char *)control + CMSG_LEN(0), &cred, cm.cmsg_len - CMSG_LEN(0));
                msg->msg_controllen = used;
            }
        }
        write_rights(msg, control, capacity, packet.rights, !!(flags & MSG_CMSG_CLOEXEC));
    }
    if (!(flags & MSG_PEEK)) {
        if (!hdr) {
            if (q->controls && q->controls->offset == 0) {
                struct stream_control *first = q->controls;
                q->controls = first->next;
                free(first);
            }
            for (struct stream_control *c = q->controls; c; c = c->next)
                c->offset -= bytes;
        }
        free_rights(packet.rights);
        size_t used = hdr ? hdr + packet.bytes : bytes;
        q->count -= used;
        memmove(q->data, q->data + used, q->count);
        wake_events();
    }
    return hdr && (flags & MSG_TRUNC) ? packet.bytes : bytes;
}

static long channel_call(CPU *cpu, int fd, u64 addr, u64 size, int writing, int flags,
                         int message) {
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_PEEK | MSG_TRUNC | MSG_CMSG_CLOEXEC))
        return -ENOTSUP;
    for (;;) {
        int snap = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
        if (event_fd(fd) != CHANNEL)
            return -EBADF;
        struct msghdr guest = {0};
        struct iovec iov[64] = {{.iov_base = (void *)addr, .iov_len = size}};
        void *output = NULL, *control = NULL;
        size_t count = 1;
        if (message) {
            output = guest_ptr(addr, sizeof guest, PROT_READ | (writing ? 0 : PROT_WRITE));
            if (!output)
                return -EFAULT;
            memcpy(&guest, output, sizeof guest);
            if (guest.msg_name || guest.msg_namelen)
                return -ENOTSUP;
            count = guest.msg_iovlen;
            if (count > sizeof iov / sizeof *iov)
                return -EMSGSIZE;
            void *vec = guest_ptr((u64)guest.msg_iov, count * sizeof *iov, PROT_READ);
            if (count && !vec)
                return -EFAULT;
            if (count)
                memcpy(iov, vec, count * sizeof *iov);
            if (guest.msg_controllen) {
                if (guest.msg_controllen > 1024)
                    return -EMSGSIZE;
                control = guest_ptr((u64)guest.msg_control, guest.msg_controllen,
                                    writing ? PROT_READ : PROT_WRITE);
                if (!control)
                    return -EFAULT;
            }
        }
        size_t total = 0;
        for (size_t i = 0; i < count; ++i) {
            if (iov[i].iov_len > INT64_MAX - total)
                return -EINVAL;
            total += iov[i].iov_len;
            iov[i].iov_base =
                guest_ptr((u64)iov[i].iov_base, iov[i].iov_len, writing ? PROT_READ : PROT_WRITE);
            if (iov[i].iov_len && !iov[i].iov_base)
                return -EFAULT;
        }
        long result = channel_transfer(fd, iov, count, total, writing, flags,
                                       message ? &guest : NULL, control);
        if (result != -EAGAIN || event_slots[fd].pair->nonblock[event_slots[fd].side] ||
            (flags & MSG_DONTWAIT)) {
            if (message && !writing && result >= 0) {
                memcpy(output, &guest, sizeof guest);
            }
            return result;
        }
        if (wait_events(cpu, snap, 0, -1) == -EINTR)
            return -EINTR;
    }
}
long channel_io(CPU *c, int fd, u64 addr, u64 size, int writing, int flags) {
    return channel_call(c, fd, addr, size, writing, flags, 0);
}
long channel_message(CPU *c, int fd, u64 addr, int writing, int flags) {
    return channel_call(c, fd, addr, 0, writing, flags, 1);
}
long event_io(CPU *cpu, int fd, u64 addr, size_t size, int writing) {
    if (event_fd(fd) == CHANNEL)
        return channel_io(cpu, fd, addr, size, writing, 0);
    if (size < 8 || event_fd(fd) != COUNTER)
        return -EINVAL;
    for (;;) {
        int snap = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
        if (event_fd(fd) != COUNTER)
            return -EBADF;
        struct event *e = &event_slots[fd];
        void *p = guest_ptr(addr, 8, writing ? PROT_READ : PROT_WRITE);
        if (!p)
            return -EFAULT;
        u64 value;
        if (writing) {
            memcpy(&value, p, 8);
            if (value == UINT64_MAX)
                return -EINVAL;
            if (UINT64_MAX - 1 - e->monitor->count >= value) {
                e->monitor->count += value;
                wake_events();
                return 8;
            }
        } else if (e->monitor->count) {
            value = (e->monitor->flags & EFD_SEMAPHORE) ? 1 : e->monitor->count;
            e->monitor->count -= value;
            memcpy(p, &value, 8);
            wake_events();
            return 8;
        }
        if (e->monitor->flags & EFD_NONBLOCK)
            return -EAGAIN;
        if (wait_events(cpu, snap, 0, -1) == -EINTR)
            return -EINTR;
    }
}
long event_fcntl(int fd, int cmd, long arg) {
    struct event *e = &event_slots[fd];
    switch (cmd) {
    case F_GETFD:
        return e->flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    case F_SETFD:
        e->flags = (e->flags & ~O_CLOEXEC) | (arg & FD_CLOEXEC ? O_CLOEXEC : 0);
        return 0;
    case F_GETFL:
        return O_RDWR | (e->pair ? e->pair->nonblock[e->side] : (e->monitor->flags & O_NONBLOCK));
    case F_SETFL:
        if (arg & ~(long)(O_NONBLOCK | O_ACCMODE))
            return -EINVAL;
        if (e->pair)
            e->pair->nonblock[e->side] = arg & O_NONBLOCK;
        else
            e->monitor->flags = (e->monitor->flags & ~O_NONBLOCK) | (arg & O_NONBLOCK);
        return 0;
    default:
        return -ENOTSUP;
    }
}
static u64 millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
long event_service(CPU *c) {
    const u64 *a = c->x;
    if (a[8] == SYS_eventfd2 || a[8] == SYS_epoll_create1) {
        int counter = a[8] == SYS_eventfd2;
        int flags = counter ? a[1] : a[0];
        if (flags & ~(counter ? (EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE) : EPOLL_CLOEXEC))
            return -EINVAL;
        int fd = free_guest_fd();
        if (fd < 0)
            return fd;
        struct monitor *m = calloc(1, sizeof *m);
        if (!m)
            return -ENOMEM;
        *m = (struct monitor){.refs = 1, .flags = flags & ~O_CLOEXEC, .count = (unsigned)a[0]};
        event_slots[fd] = (struct event){
            .kind = counter ? COUNTER : POLLSET, .flags = flags & O_CLOEXEC, .monitor = m};
        return fd;
    }
    int pollset = a[8] != SYS_ppoll;
    if (pollset && (a[0] >= GUEST_FDS || event_fd(a[0]) != POLLSET))
        return -EBADF;
    if (a[8] == SYS_epoll_ctl) {
        if (a[2] >= GUEST_FDS || (host_descriptor(a[2]) < 0 && !event_fd(a[2])))
            return -EBADF;
        if (event_fd(a[2]) == POLLSET)
            return -EINVAL;
        struct watch **p = &event_slots[a[0]].monitor->watches;
        u64 key = event_key(a[2]);
        while (*p && ((*p)->fd != (int)a[2] || (*p)->key != key))
            p = &(*p)->next;
        if (a[1] == EPOLL_CTL_DEL) {
            if (!*p)
                return -ENOENT;
            struct watch *w = *p;
            *p = w->next;
            free(w);
            return 0;
        }
        if (a[1] != EPOLL_CTL_ADD && a[1] != EPOLL_CTL_MOD)
            return -EINVAL;
        if (a[1] == EPOLL_CTL_MOD && !*p)
            return -ENOENT;
        if (a[1] == EPOLL_CTL_ADD && *p)
            return -EEXIST;
        struct epoll_event ev;
        if (!guest_copy(a[3], &ev, sizeof ev, 0))
            return -EFAULT;
        if (ev.events & ~(EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLONESHOT))
            return -ENOTSUP;
        if (!*p) {
            *p = calloc(1, sizeof **p);
            if (!*p)
                return -ENOMEM;
        }
        (*p)->fd = a[2];
        (*p)->key = key;
        (*p)->ev = ev;
        return 0;
    }
    long ms = -1;
    size_t count = pollset ? a[2] : a[1];
    if (count > GUEST_FDS || (pollset && !count))
        return -EINVAL;
    if (pollset)
        ms = (int)a[3];
    else if (a[2]) {
        struct timespec *ts = guest_ptr(a[2], sizeof *ts, PROT_READ);
        if (!ts)
            return -EFAULT;
        if (ts->tv_sec < 0 || (u64)ts->tv_nsec >= 1000000000 ||
            (u64)ts->tv_sec > INT64_MAX / 1000 - 1)
            return -EINVAL;
        ms = ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
    }
    u64 start = millis();
    for (;;) {
        int snap = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
        int has_native = 0;
        size_t ready = 0;
        if (pollset) {
            if (event_fd(a[0]) != POLLSET)
                return -EBADF;
            if (!a[1])
                return -EFAULT;
            for (struct watch *w = event_slots[a[0]].monitor->watches; w && ready < count;
                 w = w->next) {
                if (!w->ev.events)
                    continue;
                struct guest_process *saved = guest_process, *owner = saved;
                int fd = w->fd;
                if (saved->event_table->keys[fd] != w->key)
                    fd = locate_key(w->key, &owner);
                guest_process = owner;
                if (fd >= 0 && !event_fd(fd))
                    has_native = 1;
                unsigned bits = fd < 0 ? 0 : event_ready(fd, w->ev.events);
                guest_process = saved;
                if (!bits)
                    continue;
                struct epoll_event ev = w->ev;
                ev.events = bits;
                if (!guest_copy(a[1] + ready * sizeof ev, &ev, sizeof ev, 1))
                    return ready ? (long)ready : -EFAULT;
                ready++;
                if (w->ev.events & EPOLLONESHOT)
                    w->ev.events = 0;
            }
        } else {
            struct pollfd fds[GUEST_FDS];
            size_t bytes = count * sizeof *fds;
            if (!guest_copy(a[0], fds, bytes, 0))
                return -EFAULT;
            for (size_t i = 0; i < count; i++) {
                if (fds[i].fd >= 0 && !event_fd(fds[i].fd))
                    has_native = 1;
                fds[i].revents = event_ready(fds[i].fd, fds[i].events);
                ready += !!fds[i].revents;
            }
            if (!guest_copy(a[0], fds, bytes, 1))
                return -EFAULT;
        }
        if (!ready && signal_ready())
            return -EINTR;
        u64 elapsed = millis() - start;
        if (ready || (ms >= 0 && elapsed >= (u64)ms))
            return ready;
        long rem = ms >= 0 ? (long)((u64)ms - elapsed) : -1;
        wait_events(c, snap, has_native, rem);
    }
}

int channel_stat_mode(int fd) {
    struct event *e = &event_slots[fd];
    return 0600 | (e->pair ? (e->pair->pipe ? S_IFIFO : S_IFSOCK) : 0);
}
void channel_duplicate(int src, int dst, int cloexec) {
    event_slots[dst] = event_slots[src];
    event_slots[dst].flags = (event_slots[src].flags & ~O_CLOEXEC) | (cloexec ? O_CLOEXEC : 0);
    if (event_slots[dst].pair)
        ++event_slots[dst].pair->open[event_slots[dst].side];
    if (event_slots[dst].monitor)
        ++event_slots[dst].monitor->refs;
}

void events_init(void) {
    guest_process->event_table = tables = &initial_events;
    tables->owner = guest_process;
}
void events_destroy(struct guest_process *process) {
    struct guest_events *table = process->event_table;
    if (!table)
        return;
    for (unsigned fd = 0; fd < GUEST_FDS; ++fd)
        close_slot(table, fd);
    struct guest_events **link = &tables;
    while (*link != table)
        link = &(*link)->next;
    *link = table->next;
    if (table != &initial_events)
        free(table);
    process->event_table = NULL;
}
int events_clone(struct guest_process *child) {
    struct guest_events *table = calloc(1, sizeof *table);
    if (!table)
        return -ENOMEM;
    child->event_table = table;
    table->owner = child;
    table->next = tables;
    tables = table;
    for (unsigned fd = 0; fd < GUEST_FDS; ++fd) {
        if (!fd_occupied(fd))
            continue;
        table->keys[fd] = event_key(fd);
        struct event *e = &table->slots[fd];
        *e = event_slots[fd];
        if (e->pair)
            ++e->pair->open[e->side];
        if (e->monitor)
            ++e->monitor->refs;
    }
    return 0;
}
