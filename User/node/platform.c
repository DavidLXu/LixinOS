#define _GNU_SOURCE
#include <netdb.h>
#include <pthread.h>
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>
#include "uv.h"

// The Linux build tools omit this marker and use native page protections.
const char lx_node_eager_heap = 1;
extern char __end_image[];
static pthread_t main_thread;
__attribute__((constructor)) static void record_main_thread(void) {
    main_thread = pthread_self();
}
enum { launcher_stack = 2 << 20 };

int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result) {
    // The guest provides IPv4 TCP and DNS. musl's AI_ADDRCONFIG probe needs UDP.
    if (hints && (hints->ai_flags & AI_ADDRCONFIG)) {
        if (hints->ai_family == AF_INET6) return EAI_NONAME;
        if (hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET) {
            struct addrinfo ipv4 = *hints;
            ipv4.ai_flags &= ~AI_ADDRCONFIG;
            ipv4.ai_family = AF_INET;
            return __real_getaddrinfo(host, service, &ipv4, result);
        }
    }
    return __real_getaddrinfo(host, service, hints, result);
}

int __real_pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);
int __wrap_pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
    int err = __real_pthread_getattr_np(thread, attr);
    if (!err && pthread_equal(thread, main_thread))
        err = pthread_attr_setstack(attr, __end_image, launcher_stack);
    return err;
}

int __real_getrlimit(int resource, struct rlimit *limits);
int __wrap_getrlimit(int resource, struct rlimit *limits) {
    if (resource == RLIMIT_STACK) {
        limits->rlim_cur = limits->rlim_max = launcher_stack;
        return 0;
    }
    return __real_getrlimit(resource, limits);
}

uv_handle_type __real_uv_guess_handle(uv_file file);
uv_handle_type __wrap_uv_guess_handle(uv_file file) {
    uv_handle_type type = __real_uv_guess_handle(file);
    struct termios attr;
    // The guest console supports cooked text and geometry, but not termios.
    if (type == UV_TTY && tcgetattr(file, &attr) < 0) return UV_FILE;
    return type;
}

// libuv reads regular files in larger buffers than the 4 KiB guest mailbox.
// Node 22's promises.readFile treats a short regular-file read as EOF.
#include <sys/stat.h>
ssize_t __real_read(int fd, void *buffer, size_t length);
ssize_t __wrap_read(int fd, void *buffer, size_t length) {
    ssize_t first = __real_read(fd, buffer, length);
    if (first <= 0 || (size_t)first == length) return first;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) return first;
    size_t used = first;
    while (used < length) {
        ssize_t count = __real_read(fd, (char *)buffer + used, length - used);
        if (count <= 0) break;
        used += count;
    }
    return used;
}
