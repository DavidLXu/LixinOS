// Translate Linux iovec and SCM_RIGHTS records. A guest descriptor must never
// be passed to the native socket backend without translating its number.
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define CONTROL_MAX 1024
#define IOV_MAX_LOCAL 64
// Stream calls may return a short count. Translate just one guest page so
// physically independent extents work even for a large contiguous Linux buffer.
long socket_chunk(int fd, u64 addr, u64 bytes, int sending, int flags) {
    size_t room = PAGE - addr % PAGE;
    size_t len = bytes < room ? (size_t)bytes : room;
    void *p = len ? guest_ptr(addr, len, sending ? PROT_READ : PROT_WRITE) : NULL;
    if (len && !p)
        return -EFAULT;
    ssize_t n =
        sending ? send(fd, p, len, flags | MSG_DONTWAIT) : recv(fd, p, len, flags | MSG_DONTWAIT);
    return n < 0 ? -errno : n;
}
static long transfer(CPU *cpu, int fd, int sending, int flags) {
    const u64 *a = cpu->x;
    struct msghdr raw = {0}, guest = {0};
    struct iovec iov[IOV_MAX_LOCAL];
    union {
        struct cmsghdr align;
        unsigned char bytes[CONTROL_MAX];
    } ancillary;
    int single = a[8] == SYS_sendto || a[8] == SYS_recvfrom;
    void *msg_dst = NULL, *ctrl_dst = NULL;
    int access = sending ? PROT_READ : PROT_WRITE;
    if (single) {
        if (a[4] || a[5])
            return -ENOTSUP;
        return socket_chunk(fd, a[1], a[2], sending, flags);
    } else {
        msg_dst = guest_ptr(a[1], sizeof guest, PROT_READ | (sending ? 0 : PROT_WRITE));
        if (!msg_dst)
            return -EFAULT;
        memcpy(&guest, msg_dst, sizeof guest);
        if (guest.msg_name || guest.msg_namelen)
            return -ENOTSUP;
        if (guest.msg_iovlen > IOV_MAX_LOCAL || guest.msg_controllen > CONTROL_MAX)
            return -EMSGSIZE;
        size_t bytes = guest.msg_iovlen * sizeof(*iov);
        void *table = guest_ptr((u64)guest.msg_iov, bytes, PROT_READ);
        if (bytes && !table)
            return -EFAULT;
        if (bytes)
            memcpy(iov, table, bytes);
        if (socket_family(a[0]) == AF_INET) {
            if (sending && guest.msg_controllen)
                return -ENOTSUP;
            long count = 0;
            for (size_t i = 0; i < guest.msg_iovlen; i++) {
                if (!iov[i].iov_len)
                    continue;
                count = socket_chunk(fd, (u64)iov[i].iov_base, iov[i].iov_len, sending, flags);
                break;
            }
            if (!sending && count >= 0) {
                guest.msg_flags = 0;
                guest.msg_controllen = 0;
                memcpy(msg_dst, &guest, sizeof guest);
            }
            return count;
        }
        for (size_t i = 0; i < guest.msg_iovlen; i++) {
            iov[i].iov_base = guest_ptr((u64)iov[i].iov_base, iov[i].iov_len, access);
            if (iov[i].iov_len && !iov[i].iov_base)
                return -EFAULT;
        }
        raw.msg_iov = iov;
        raw.msg_iovlen = guest.msg_iovlen;
        if (guest.msg_controllen) {
            ctrl_dst = guest_ptr((u64)guest.msg_control, guest.msg_controllen, access);
            if (!ctrl_dst)
                return -EFAULT;
            raw.msg_control = ancillary.bytes;
            raw.msg_controllen = guest.msg_controllen;
            if (sending) {
                memcpy(ancillary.bytes, ctrl_dst, guest.msg_controllen);
                size_t off = 0;
                while (off < raw.msg_controllen) {
                    size_t left = raw.msg_controllen - off;
                    if (left < sizeof(struct cmsghdr))
                        return -EINVAL;
                    struct cmsghdr *cm = (void *)(ancillary.bytes + off);
                    if (cm->cmsg_len < CMSG_LEN(0) || cm->cmsg_len > left ||
                        (cm->cmsg_len - CMSG_LEN(0)) % sizeof(int))
                        return -EINVAL;
                    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
                        return -ENOTSUP;
                    int *fds = (void *)CMSG_DATA(cm);
                    for (size_t i = 0; i < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
                        fds[i] = export_native_fd(fds[i]);
                        if (fds[i] < 0)
                            return -EBADF;
                    }
                    off += CMSG_ALIGN(cm->cmsg_len);
                }
            }
        }
    }
    ssize_t n =
        sending ? sendmsg(fd, &raw, flags | MSG_DONTWAIT) : recvmsg(fd, &raw, flags | MSG_DONTWAIT);
    if (n < 0)
        return -errno;
    if (!sending && !single) {
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&raw); cm; cm = CMSG_NXTHDR(&raw, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
                continue;
            int *fds = (void *)CMSG_DATA(cm);
            for (size_t i = 0; i < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
                long guestfd = adopt_fd(fds[i]);
                if (guestfd < 0)
                    raw.msg_flags |= MSG_CTRUNC;
                fds[i] = guestfd;
            }
        }
        guest.msg_flags = raw.msg_flags;
        guest.msg_controllen = raw.msg_controllen;
        if (raw.msg_controllen)
            memcpy(ctrl_dst, ancillary.bytes, raw.msg_controllen);
        memcpy(msg_dst, &guest, sizeof guest);
    }
    return n;
}
long message_service(CPU *c) {
    const u64 *a = c->x;
    int single = a[8] == SYS_sendto || a[8] == SYS_recvfrom;
    int sending = a[8] == SYS_sendmsg || a[8] == SYS_sendto;
    int flags = single ? a[3] : a[2];
    if (a[0] < GUEST_FDS && event_fd(a[0])) {
        if (!single)
            return channel_message(c, a[0], a[1], sending, flags);
        if (a[4] || a[5])
            return -ENOTSUP;
        return channel_io(c, a[0], a[1], a[2], sending, flags);
    }
    for (;;) {
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        long n = transfer(c, fd, sending, flags);
        if (n != -EAGAIN || (flags & MSG_DONTWAIT) || (fcntl(fd, F_GETFL) & O_NONBLOCK))
            return n;
        thread_pause();
        usleep(1000);
        thread_resume(c);
    }
}
