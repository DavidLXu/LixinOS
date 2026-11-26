// Start the Tencent Linux client on a private socket of the native compositor.
#define _GNU_SOURCE
#include "app-state.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(void) {
    char *legacy[] = {"/wx-state", "/wx-state/home", "/wx-state/run"};
    char *writeback[] = {"/scratch/wx-state", "/scratch/wx-state/home", "/scratch/wx-state/run"};
    char **dirs = app_state_dirs(legacy, writeback);
    for (size_t i = 0; i < sizeof legacy / sizeof *legacy; i++) {
        if (mkdir(dirs[i], 0700) < 0 && errno != EEXIST) {
            perror(dirs[i]);
            return 1;
        }
    }
    if (setenv("HOME", dirs[1], 1) || setenv("XDG_RUNTIME_DIR", dirs[2], 1) ||
        setenv("GSETTINGS_SCHEMA_DIR", "/lib/gnome/schemas", 1) ||
        setenv("GSETTINGS_BACKEND", "memory", 1)) {
        perror("wechat");
        return 1;
    }
    char *argv[] = {"/bin/dbus-run-session",
                    "--",
                    "/lib/wechat-ui/mutter",
                    "--lixinos-display",
                    "--wayland-display",
                    "wayland-0",
                    "--",
                    "/bin/linux-run",
                    "--state",
                    dirs[0],
                    "/opt/wechat/wechat",
                    NULL};
    pid_t child;
    int rc = posix_spawn(&child, argv[0], NULL, NULL, argv, environ);
    if (rc) {
        fprintf(stderr, "wechat: %s\n", strerror(rc));
        return 1;
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("wechat");
        return 1;
    }
    if (dirs == writeback && app_state_sync(dirs[0]) < 0) {
        perror("wechat profile");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
