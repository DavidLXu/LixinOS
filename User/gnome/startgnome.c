#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int configure_session(const char *runtime)
{
    const struct {
        const char *name;
        const char *value;
        int replace;
    } settings[] = {
        {"XDG_RUNTIME_DIR", runtime, 1},
        {"XDG_DATA_DIRS", "/lib/gnome/share", 0},
        {"XDG_SESSION_TYPE", "wayland", 1},
        {"XDG_CURRENT_DESKTOP", "GNOME", 1},
        {"GSETTINGS_SCHEMA_DIR", "/lib/gnome/schemas", 1},
        {"GSETTINGS_BACKEND", "keyfile", 0},
        {"FONTCONFIG_FILE", "/lib/gnome/fonts.conf", 0},
        {"XCURSOR_PATH", "/lib/gnome/share/icons", 0},
        {"LIBGWEATHER_LOCATIONS_PATH", "/lib/gnome/Locations.bin", 0},
        {"GSK_RENDERER", "cairo", 0},
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (setenv(settings[i].name, settings[i].value, settings[i].replace) < 0)
            return errno;
    }
    return 0;
}

int main(void)
{
    char runtime[] = "/tmp/lx-gnome-XXXXXX";
    if (!mkdtemp(runtime)) {
        perror("startgnome: create runtime directory");
        return 1;
    }

    char *args[] = {"/bin/dbus-run-session", "--private-system-bus", "--",
                    "/bin/gnome-shell", "--lixinos-display", NULL};
    pid_t child;
    int status = 0;
    int error = configure_session(runtime);
    if (!error)
        error = posix_spawn(&child, args[0], NULL, NULL, args, environ);
    if (!error) {
        pid_t result;
        do {
            result = waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            error = errno;
    }
    rmdir(runtime);
    if (error) {
        fprintf(stderr, "startgnome: %s\n", strerror(error));
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
