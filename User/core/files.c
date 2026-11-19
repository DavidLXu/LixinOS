// Rebuild with: tcc /src/core/cp.c -o /bin/cp
#include <lixin.h>
#include <string.h>

#define MAIL ((char *)0x443d7000UL)
#define PATH_SIZE 513

enum Operation { BEGIN, CREATE_DIR, CREATE_FILE, UNLINK, COPY_FILE, RENAME, COMMIT, ABORT, EXISTS };

long lx_console_call(long operation, long value);

static int fail(const char *error) {
    lx_error(error);
    lx_error("\n");
    return 1;
}

static int edit(enum Operation operation, int flag, const char *path, const char *other) {
    if (path) strcpy(MAIL, path);
    if (other) strcpy(MAIL + PATH_SIZE, other);
    if (lx_console_call(45, operation | flag << 8) < 0)
        return fail(MAIL);
    return 0;
}

static int inside(const char *path, const char *root) {
    unsigned len = strlen(root);
    return !strncmp(path, root, len) && (!path[len] || path[len] == '/');
}

static int join(char path[PATH_SIZE], unsigned length, const char *name) {
    unsigned slash = length > 1;
    unsigned size = strlen(name);
    if (length + slash + size >= PATH_SIZE) return fail("path too long");
    if (slash) path[length++] = '/';
    strcpy(path + length, name);
    return 0;
}

// Normalize incrementally so mkdir -p can stage missing ancestors and
// rm -f distinguishes a missing ancestor from a path through a file.
static int resolve(const char *cwd, const char *input, char path[PATH_SIZE],
                   int parents, int force) {
    if (!*input) return fail("empty path");
    strcpy(path, *input == '/' ? "/" : cwd);
    long kind = LX_DIR;
    while (*input) {
        if (*input == '/') { ++input; continue; }
        if (kind != LX_DIR) return fail("not a directory");
        const char *end = input;
        while (*end && *end != '/') ++end;
        unsigned len = end - input;
        if (len == 2 && input[0] == '.' && input[1] == '.') {
            char *slash = strrchr(path, '/');
            slash[slash == path] = 0;
        } else if (!(len == 1 && input[0] == '.')) {
            unsigned pos = strlen(path);
            if (pos + (pos > 1) + len >= PATH_SIZE) return fail("path too long");
            if (pos > 1) path[pos++] = '/';
            memcpy(path + pos, input, len);
            path[pos + len] = 0;
            if (parents) {
                if (edit(CREATE_DIR, 1, path, 0)) return 1;
            } else {
                kind = lx_stat(path);
                if (kind == LX_MISSING && force) return 2;
                // Absent final components are valid paths for new entries.
                if (kind < 0 && kind != LX_DIR && kind != LX_MISSING)
                    return fail("not a directory");
            }
        }
        input = end;
    }
    return 0;
}

static int walk(char source[PATH_SIZE], char dest[PATH_SIZE], int copy) {
    struct Level { unsigned src, dst, next; } levels[257];
    char name[PATH_SIZE];
    unsigned depth = 1;
    levels[0] = (struct Level){strlen(source), copy ? strlen(dest) : 0, 0};
    if (copy && edit(CREATE_DIR, 0, dest, 0)) return 1;
    while (depth) {
        struct Level *level = &levels[depth - 1];
        source[level->src] = 0;
        if (copy) dest[level->dst] = 0;
        long kind = lx_dir(source, level->next++, name);
        if (kind == LX_END) {
            if (!copy && edit(UNLINK, 1, source, 0)) return 1;
            --depth;
        } else if (kind >= 0 || kind == LX_DIR) {
            if (join(source, level->src, name) || (copy && join(dest, level->dst, name))) return 1;
            if (kind == LX_DIR) {
                if (depth == sizeof(levels) / sizeof(*levels)) return fail("directory nesting too deep");
                if (copy && edit(CREATE_DIR, 0, dest, 0)) return 1;
                levels[depth++] = (struct Level){strlen(source), copy ? strlen(dest) : 0, 0};
            } else if (edit(copy ? COPY_FILE : UNLINK, 0, source, copy ? dest : 0)) return 1;
        } else {
            return fail("directory read failed");
        }
    }
    return 0;
}

static int touch_file(const char *cwd, const char *name) {
    char path[PATH_SIZE];
    if (resolve(cwd, name, path, 0, 0)) return 1;
    return !strcmp(path, "/") ? 0 : edit(CREATE_FILE, 0, path, 0);
}

static int touch_manifest(const char *cwd, const char *manifest) {
    int fd = lx_open(manifest);
    if (fd < 0) return fail("file not found");
    char data[4096], name[PATH_SIZE + 1];
    unsigned len = 0;
    int n, status = 0;
    while (!status && (n = lx_read(fd, data, sizeof(data))) > 0) {
        for (int i = 0; !status && i < n; ++i) {
            char byte = data[i];
            if (byte == '\n') {
                if (len && name[len - 1] == '\r') --len;
                name[len] = 0;
                if (len) status = touch_file(cwd, name);
                len = 0;
            } else if (!byte || len == PATH_SIZE) {
                status = fail("invalid path in manifest");
            } else {
                name[len++] = byte;
            }
        }
    }
    if (!status && n < 0) status = fail("manifest read failed");
    if (!status && len) { name[len] = 0; status = touch_file(cwd, name); }
    lx_close(fd);
    return status;
}

#define STR_(x) #x
#define STR(x) STR_(x)

static int run(const char *cwd, int argc, char **argv) {
    const char *command = STR(APP);
    int cp = !strcmp(command, "cp"), mv = !strcmp(command, "mv");
    int rm = !strcmp(command, "rm"), rmdir = !strcmp(command, "rmdir");
    int mkdir = !strcmp(command, "mkdir"), touch = !strcmp(command, "touch");
    if (touch && argc == 3 && !strcmp(argv[1], "--files-from"))
        return touch_manifest(cwd, argv[2]);

    int parents = 0, recursive = 0, force = 0, no_clobber = 0, options = 1;
    const char *operands[512];
    int count = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (options && !strcmp(arg, "--")) { options = 0; continue; }
        if (options && *arg == '-' && arg[1]) {
            for (++arg; *arg; ++arg) {
                if (mkdir && *arg == 'p') parents = 1;
                else if ((cp || rm) && (*arg == 'r' || *arg == 'R')) recursive = 1;
                else if (rm && *arg == 'f') force = 1;
                else if ((cp || mv) && *arg == 'n') no_clobber = 1;
                else return fail("unsupported option");
            }
        } else operands[count++] = arg;
    }
    char source[PATH_SIZE], target[PATH_SIZE], destination[PATH_SIZE];
    if (cp || mv) {
        if (count < 2) return fail("expected source and destination");
        if (resolve(cwd, operands[--count], destination, 0, 0)) return 1;
        int into_dir = lx_stat(destination) == LX_DIR;
        if (count > 1 && !into_dir) return fail("destination must be a directory");
        for (int i = 0; i < count; ++i) {
            if (resolve(cwd, operands[i], source, 0, 0)) return 1;
            if (!strcmp(source, "/")) return fail("cannot copy or move the root directory");
            long kind = lx_stat(source);
            if (kind < 0 && kind != LX_DIR) return fail("file not found");
            strcpy(target, destination);
            if (into_dir && join(target, strlen(target), strrchr(source, '/') + 1)) return 1;
            if (inside(target, source) || inside(source, target)) return fail("source and destination overlap");
            if (no_clobber) {
                strcpy(MAIL, target);
                long exists = lx_console_call(45, EXISTS);
                if (exists < 0) return fail(MAIL);
                if (exists) continue;
            }
            if (mv) {
                if (edit(RENAME, no_clobber, source, target)) return 1;
            } else if (kind == LX_DIR) {
                if (!recursive) return fail("use cp -r for directories");
                if (walk(source, target, 1)) return 1;
            } else if (edit(COPY_FILE, no_clobber, source, target)) return 1;
        }
        return 0;
    }
    if (!count && !force) return fail("expected a path");
    for (int i = 0; i < count; ++i) {
        if (touch) {
            if (touch_file(cwd, operands[i])) return 1;
            continue;
        }
        int status = resolve(cwd, operands[i], source, parents, force);
        if (status == 2) continue;
        if (status) return 1;
        if (mkdir) {
            if (!parents && edit(CREATE_DIR, 0, source, 0)) return 1;
        } else if (recursive && lx_stat(source) == LX_DIR) {
            if (!strcmp(source, "/") || inside(cwd, source))
                return fail("cannot remove the working directory or root");
            if (walk(source, target, 0)) return 1;
        } else if (edit(UNLINK, rmdir, source, 0)) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char cwd[PATH_SIZE];
    long length = lx_console_call(42, 0);
    if (length <= 0 || length >= PATH_SIZE) return fail("cannot read working directory");
    memcpy(cwd, MAIL, length);
    cwd[length] = 0;
    if (edit(BEGIN, 0, 0, 0)) return 1;
    if (run(cwd, argc, argv)) {
        edit(ABORT, 0, 0, 0);
        return 1;
    }
    return edit(COMMIT, 0, 0, 0);
}
