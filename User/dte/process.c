// Replace the upstream fork/exec sequence with LixinOS's spawn adapter.
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "util/xmalloc.h"

extern char **environ;

static char **child_environment(const char **overrides)
{
    size_t count = 0;
    for (char **entry = environ; *entry; ++entry) {
        count++;
    }
    size_t capacity = count + 1;
    for (size_t i = 0; overrides[i]; i += 2) {
        capacity++;
    }
    char **result = xnew0(char *, capacity);
    for (size_t i = 0; i < count; i++) {
        result[i] = xstrdup(environ[i]);
    }
    for (size_t i = 0; overrides[i]; i += 2) {
        const char *name = overrides[i];
        const char *value = overrides[i + 1];
        size_t len = strlen(name);
        size_t slot = 0;
        while (slot < count && (strncmp(result[slot], name, len) || result[slot][len] != '=')) {
            slot++;
        }
        free(result[slot]);
        if (value) {
            result[slot] = xasprintf("%s=%s", name, value);
            count += slot == count;
        } else if (slot < count) {
            result[slot] = result[--count];
            result[count] = NULL;
        }
    }
    return result;
}

pid_t __wrap_fork_exec(const char **argv, const char **env, int fd[3], bool drop_ctty)
{
    // The caller routes the child's standard streams to its terminal, pipes,
    // or /dev/null. The guest has no separate controlling-terminal state.
    (void)drop_ctty;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        errno = error;
        return -1;
    }
    for (int i = 0; i < 3 && !error; i++) {
        if (fd[i] != i) {
            error = posix_spawn_file_actions_adddup2(&actions, fd[i], i);
        }
    }
    char **environment = env ? child_environment(env) : environ;
    pid_t pid;
    if (!error) {
        error = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environment);
    }
    if (env) {
        for (char **entry = environment; *entry; ++entry) {
            free(*entry);
        }
        free(environment);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        errno = error;
        return -1;
    }
    return pid;
}
