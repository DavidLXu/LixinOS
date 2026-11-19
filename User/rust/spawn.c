// musl file actions are documented in its private fdop.h. This adapter is
// compiled against the same pinned musl sources as the C sysroot.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "fdop.h"
#include "../console.h"

extern int lx_child_descriptor(int, int);
extern int lx_record_child(int);
static int append(char *mail, size_t *used, const char *word) {
    size_t n = strlen(word) + 1;
    if (n > 32768 - *used) return E2BIG;
    memcpy(mail + *used, word, n);
    *used += n;
    return 0;
}

static int spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const env[], int search) {
    if (!argv || !argv[0]) return EINVAL;
    if (attr && (attr->__flags & ~(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK))) return ENOTSUP;
    char cwd[513], resolved[513];
    if (!getcwd(cwd, sizeof cwd)) return errno;
    int streams[] = {0, 1, 2};
    struct fdop *op = actions ? actions->__actions : NULL;
    while (op && op->next) op = op->next;
    for (; op; op = op->prev) {
        if (op->cmd == FDOP_DUP2 && (unsigned)op->fd < 3) {
            int source = op->srcfd;
            if ((unsigned)source < 3) source = streams[source];
            streams[op->fd] = source;
        } else if (op->cmd == FDOP_CHDIR) {
            if (op->path[0] == '/') {
                if (strlen(op->path) >= sizeof cwd) return ENAMETOOLONG;
                strcpy(cwd, op->path);
            } else {
                size_t n = strlen(cwd);
                if (n + strlen(op->path) + 2 > sizeof cwd) return ENAMETOOLONG;
                cwd[n] = '/'; strcpy(cwd + n + 1, op->path);
            }
        } else if (op->cmd != FDOP_CLOSE || op->fd < 3) return ENOTSUP;
    }
    if (search && !strchr(path, '/')) {
        const char *dirs = getenv("PATH");
        if (!dirs) dirs = "/bin";
        for (;;) {
            size_t n = strcspn(dirs, ":");
            if (snprintf(resolved,sizeof resolved,"%.*s%s%s",(int)n,dirs,n ? "/" : "",path) < (int)sizeof resolved
                && access(resolved, X_OK) == 0) { path = resolved; break; }
            if (!dirs[n]) return ENOENT;
            dirs += n + 1;
        }
    }
    uint64_t flags = 1UL << 24;
    for (int i = 0; i < 3; i++) {
        int raw = lx_child_descriptor(streams[i], i);
        if (raw < 0) return ENOTSUP;
        flags |= (uint64_t)raw << (i * 8);
    }
    char *mail = (void *)0x443d8000;
    size_t used = 2;
    for (unsigned i = 0; env && env[i]; i++) {
        if (i >= 256 || append(mail, &used, env[i]) || used-2 > 8192) return E2BIG;
    }
    mail[0] = (used-2) & 255; mail[1] = (used-2) >> 8;
    if (append(mail, &used, cwd) || append(mail, &used, path)) return E2BIG;
    for (unsigned i = 0; argv[i]; i++) {
        if (i >= 512 || append(mail, &used, argv[i])) return E2BIG;
    }
    long child = console_call(50, (uint64_t)used << 32 | flags);
    if (child < 0) return ENOEXEC;
    *pid = child;
    return lx_record_child(child) < 0 ? EAGAIN : 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const env[]) {
    return spawn(pid, path, actions, attr, argv, env, 0);
}
int posix_spawnp(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const env[]) {
    return spawn(pid, path, actions, attr, argv, env, 1);
}
