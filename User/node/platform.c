#define _GNU_SOURCE
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
