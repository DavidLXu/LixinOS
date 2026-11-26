// Start Chromium with its own native Wayland and D-Bus session.
#define _GNU_SOURCE
#include "../linux-run/app-state.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    char *legacy[] = {"/chromium-state", "/chromium-state/home", "/chromium-state/run"};
    char *writeback[] = {"/scratch/chromium-state", "/scratch/chromium-state/home",
                         "/scratch/chromium-state/run"};
    char **dirs = app_state_dirs(legacy, writeback);
    for (size_t i = 0; i < sizeof legacy / sizeof *legacy; ++i) {
        if (mkdir(dirs[i], 0700) < 0 && errno != EEXIST) {
            perror(dirs[i]);
            return 1;
        }
    }
    if (setenv("HOME", dirs[1], 1) || setenv("XDG_RUNTIME_DIR", dirs[2], 1) ||
        setenv("GSETTINGS_SCHEMA_DIR", "/lib/gnome/schemas", 1) ||
        setenv("GSETTINGS_BACKEND", "memory", 1)) {
        perror("chromium");
        return 1;
    }

    char *compositor = "/bin/mutter";
    if (access(compositor, X_OK) < 0)
        compositor = "/lib/wechat-ui/mutter";
    // The software compositor can submit its shared images in the browser
    // process. Page renderers and the network service keep their own processes.
    char *prefix[] = {"/bin/dbus-run-session", "--", compositor, "--lixinos-display",
                      "--wayland-display", "wayland-0", "--", "/bin/linux-run",
                      "--processes", "--state", dirs[0], "/opt/chromium/content_shell",
                      "--ozone-platform=wayland", "--in-process-gpu",
                      // Drop a disconnected HTTP/2 session from the pool before reconnecting.
                      "--enable-features=DrainSpdySessionSynchronouslyOnRemoteEndpointDisconnect"};
    size_t count = sizeof prefix / sizeof *prefix;
    char **args = calloc(count + argc + 1, sizeof *args);
    if (!args) {
        perror("chromium");
        return 1;
    }
    memcpy(args, prefix, sizeof prefix);
    if (argc == 1)
        args[count] = "about:blank";
    else
        memcpy(args + count, argv + 1, (argc - 1) * sizeof *args);

    pid_t child;
    int rc = posix_spawn(&child, args[0], NULL, NULL, args, environ);
    free(args);
    if (rc) {
        fprintf(stderr, "chromium: %s\n", strerror(rc));
        return 1;
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("chromium");
        return 1;
    }
    if (dirs == writeback && app_state_sync(dirs[0]) < 0) {
        perror("chromium profile");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
