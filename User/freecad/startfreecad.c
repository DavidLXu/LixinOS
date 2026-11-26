#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef APP_NAME
#define APP_NAME "freecad"
#define APP_COMPOSITOR "/lib/freecad-desktop/mutter"
#define APP_PROGRAM "/lib/freecad-desktop/FreeCAD"
#define APP_DISPLAY "wayland-cad"
#endif

extern char **environ;

static int run(char **args)
{
    pid_t child;
    int status;
    int error = posix_spawn(&child, args[0], NULL, NULL, args, environ);
    if (error) {
        fprintf(stderr, APP_NAME ": %s: %s\n", args[0], strerror(error));
        return 1;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror(APP_NAME ": waitpid");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv)
{
    if (!getenv("QT_QPA_FONTDIR") && access("/usr/share/fonts/FiraSans-Regular.ttf", R_OK) == 0)
        setenv("QT_QPA_FONTDIR", "/usr/share/fonts", 0);
    const char *display = getenv("WAYLAND_DISPLAY");
    const char *socket = getenv("WAYLAND_SOCKET");
    char *program = APP_PROGRAM;
    if ((display && *display) || (socket && *socket)) {
        argv[0] = program;
        return run(argv);
    }

    char runtime[] = "/tmp/" APP_NAME "-XXXXXX";
    char *prefix[] = {"/bin/dbus-run-session", "--", APP_COMPOSITOR,
                      "--lixinos-display", "--wayland-display", APP_DISPLAY, "--", program};
    const size_t count = sizeof(prefix) / sizeof(prefix[0]);
    char **args = calloc(count + argc, sizeof(*args));
    if (!args || !mkdtemp(runtime)) {
        perror(APP_NAME);
        free(args);
        return 1;
    }
    memcpy(args, prefix, sizeof(prefix));
    memcpy(args + count, argv + 1, argc * sizeof(*args));
    int status = 1;
    if (
#ifdef APP_SINGLE_CLIENT
        setenv("LIXINOS_SINGLE_CLIENT", "1", 1) ||
#endif
        setenv("HOME", "/", 0) || setenv("PATH", "/bin", 0) ||
        setenv("XDG_RUNTIME_DIR", runtime, 1) ||
        setenv("GSETTINGS_SCHEMA_DIR", "/lib/gnome/schemas", 1) ||
        setenv("GSETTINGS_BACKEND", "memory", 1) ||
        setenv("WAYLAND_DISPLAY", APP_DISPLAY, 1) ||
        setenv("QT_QPA_PLATFORM", "wayland", 1) ||
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1))
        perror(APP_NAME ": configure session");
    else
        status = run(args);
    free(args);
    rmdir(runtime);
    return status;
}
