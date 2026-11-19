// libiberty's process backend for GCC running on LixinOS.
#include "pex-common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "console.h"

extern int lx_child_descriptor(int, int);

static int open_read(struct pex_obj *obj, const char *name, int binary) {
    return open(name, O_RDONLY);
}
static int open_write(struct pex_obj *obj, const char *name, int binary) {
    return open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}
static int close_fd(struct pex_obj *obj, int fd) { return close(fd); }
static int pipe_fd(struct pex_obj *obj, int *fds, int binary) { return pipe(fds); }
static FILE *read_stream(struct pex_obj *obj, int fd, int binary) { return fdopen(fd, "r"); }
static FILE *write_stream(struct pex_obj *obj, int fd, int binary) { return fdopen(fd, "w"); }

static pid_t spawn_child(struct pex_obj *obj, int flags, const char *path,
                         char *const *argv, char *const *env, int in, int out,
                         int errfd, int toclose, const char **message, int *error) {
    int fds[] = {in, out, flags & PEX_STDERR_TO_STDOUT ? out : errfd};
    uint64_t streams = 0;
    char cwd[513], resolved[513], *mail = (void *)0x443d8000;
    size_t used = 0;
    *message = "spawn";
    if (!getcwd(cwd, sizeof cwd)) { *error = errno; return -1; }
    if (!strchr(path, '/') && flags & PEX_SEARCH) {
        if (snprintf(resolved, sizeof resolved, "/bin/%s", path) >= (int)sizeof resolved) {
            *error = ENAMETOOLONG; return -1;
        }
        path = resolved;
    }
    for (int i = 0; i != 3; ++i) {
        int raw = lx_child_descriptor(fds[i], i);
        if (raw < 0) { *error = ENOTSUP; return -1; }
        streams |= (uint64_t)raw << (8*i);
    }
    for (int i = -2; i < 0 || argv[i]; ++i) {
        const char *arg = i == -2 ? cwd : i == -1 ? path : argv[i];
        size_t n = strlen(arg) + 1;
        if (i >= 512 || n > 32768-used) { *error = E2BIG; return -1; }
        memcpy(mail+used, arg, n);
        used += n;
    }
    long pid = console_call(50, ((uint64_t)used << 32) | streams);
    if (pid < 0) { *error = ENOEXEC; return -1; }
    if (in != 0) close(in);
    if (out != 1) close(out);
    if (errfd != 2 && errfd != out) close(errfd);
    return pid;
}
static pid_t wait_child(struct pex_obj *obj, pid_t pid, int *status,
                        struct pex_time *time, int done, const char **msg, int *error) {
    long result = console_call(51, (unsigned)pid);
    if (result < 0) { *msg = "wait"; *error = ECHILD; return -1; }
    *status = (result & 255) << 8;
    if (time) memset(time, 0, sizeof *time);
    return 0;
}
struct pex_obj *pex_init(int flags, const char *name, const char *base) {
    static const struct pex_funcs funcs = {open_read, open_write, spawn_child, close_fd,
        wait_child, pipe_fd, read_stream, write_stream, NULL};
    return pex_init_common(flags, name, base, &funcs);
}
