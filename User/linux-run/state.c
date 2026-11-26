// Optional private home, temporary files and runtime directory for a Linux program.
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root;
static int chromium_fonts;
static struct {
    const char *linux_dir;
    const char *private_dir;
} mounts[] = {{"/home/wechat", "/home"}, {"/tmp", "/tmp"}, {"/run/user/1000", "/run"}};

static int clean_path(const char *path) {
    if (*path != '/')
        return 0;
    for (const char *s = path; *s;) {
        while (*s == '/')
            ++s;
        size_t n = strcspn(s, "/");
        if ((n == 1 && *s == '.') || (n == 2 && !strncmp(s, "..", 2)))
            return 0;
        s += n;
    }
    return 1;
}

int state_path(const char *path, char out[513]) {
    if (!root || !path || !clean_path(path))
        return 0;
    for (size_t i = 0; i < sizeof mounts / sizeof *mounts; ++i) {
        size_t n = strlen(mounts[i].linux_dir);
        if (strncmp(path, mounts[i].linux_dir, n) || (path[n] && path[n] != '/'))
            continue;
        int len = snprintf(out, 513, "%s%s%s", root, mounts[i].private_dir, path + n);
        return len >= 0 && len < 513;
    }
    return 0;
}

// Resolve a mutable entry below a mounted state directory. A relative name
// starts at an already-open private directory in the native filesystem.
int state_entry(const char *name, const char *directory, char out[513]) {
    if (!root || !*name)
        return -ENOENT;
    char absolute[513];
    if (*name != '/' && !directory) {
        if (snprintf(absolute, sizeof absolute, "/%s", name) >= (int)sizeof absolute)
            return -ENAMETOOLONG;
        name = absolute; // The Linux working directory is the virtual root.
    }
    if (*name == '/') {
        if (!state_path(name, out))
            return -EACCES;
        for (size_t i = 0; i < sizeof mounts / sizeof *mounts; ++i) {
            size_t n = strlen(mounts[i].linux_dir);
            if (!strncmp(name, mounts[i].linux_dir, n) && !name[n + strspn(name + n, "/")])
                return -EBUSY;
        }
        return 0;
    }
    int n = snprintf(out, 513, "%s/%s", directory, name);
    if (n < 0 || n >= 513)
        return -ENAMETOOLONG;
    return clean_path(out) ? 0 : -EACCES;
}

void state_init(const char *path, const char *home) {
    mounts[0].linux_dir = home;
    if (!clean_path(path) || !path[1] || strlen(path) > 480)
        fail("invalid state directory");
    root = path;
    chromium_fonts = !strcmp(home, "/home/chromium") && !access("/opt/chromium/fonts.conf", R_OK);
    struct stat st;
    if (stat(root, &st) || !S_ISDIR(st.st_mode))
        fail("state directory must already exist");
    for (size_t i = 0; i < sizeof mounts / sizeof *mounts; ++i) {
        char dir[513];
        if (!state_path(mounts[i].linux_dir, dir) || (mkdir(dir, 0700) && errno != EEXIST) ||
            stat(dir, &st) || !S_ISDIR(st.st_mode))
            fail("cannot prepare private state directory");
    }
}

int state_parent(const char *s) {
    return root && (!strcmp(s, "/") || !strcmp(s, "/home") || !strcmp(s, "/run") ||
                    !strcmp(s, "/run/user"));
}

// The Linux client reads the same keyboard maps and fonts as native Wayland clients.
int state_resource(const char *path, char out[513]) {
    static const char *const system_files[] = {
        "/proc/cpuinfo", "/proc/meminfo", "/proc/stat", "/proc/uptime", "/proc/version",
    };
    for (size_t i = 0; i < sizeof system_files / sizeof *system_files; ++i) {
        if (!strcmp(path, system_files[i])) {
            strcpy(out, path);
            return 1;
        }
    }
    if (!root || !clean_path(path))
        return 0;
    if (chromium_fonts) {
        if (!strcmp(path, "/etc/fonts/fonts.conf")) {
            strcpy(out, "/opt/chromium/fonts.conf");
            return 1;
        }
        const char *dir = "/opt/chromium/fonts";
        size_t n = strlen(dir);
        if (!strcmp(path, "/lib/gnome/fonts.conf") ||
            (!strncmp(path, dir, n) && (!path[n] || path[n] == '/'))) {
            return snprintf(out, 513, "%s", path) < 513;
        }
    }
    static const struct {
        const char *guest, *native;
    } resources[] = {
        {"/usr/share/X11/xkb", "/lib/gnome/xkb"},
        {"/etc/nsswitch.conf", "/lib/linux-run/nsswitch.conf"},
        {"/etc/fonts/fonts.conf", "/lib/gnome/fonts.conf"},
        {"/usr/share/fonts", "/usr/share/fonts"},
        {"/lib/gnome/fonts", "/lib/gnome/fonts"},
    };
    for (size_t i = 0; i < sizeof resources / sizeof *resources; ++i) {
        size_t len = strlen(resources[i].guest);
        if (strncmp(path, resources[i].guest, len) || (path[len] && path[len] != '/'))
            continue;
        int n = snprintf(out, 513, "%s%s", resources[i].native, path + len);
        return n >= 0 && n < 513;
    }
    return 0;
}
