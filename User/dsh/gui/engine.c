#define _GNU_SOURCE
#include "engine.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pid_t child;
static int stream[2];
static GString *partial[2];
static DshLine on_line;
static DshDone on_done;

gboolean dsh_busy(void) {
    return child > 0;
}

/* Drain a bounded amount each tick so a busy tool cannot starve the window. */
static gboolean pump(gpointer unused) {
    (void)unused;
    char buf[4096];
    for (int i = 0; i < 2; i++) {
        for (int n = 0; stream[i] >= 0 && n < 32; n++) {
            ssize_t count = read(stream[i], buf, sizeof buf);
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            if (count <= 0) {
                if (partial[i]->len)
                    on_line(partial[i]->str, i != 0);
                close(stream[i]);
                stream[i] = -1;
                break;
            }
            const char *p = buf, *end = buf + count;
            while (p < end) {
                const char *newline = memchr(p, '\n', end - p);
                g_string_append_len(partial[i], p, (newline ? newline : end) - p);
                if (!newline)
                    break;
                on_line(partial[i]->str, i != 0);
                g_string_truncate(partial[i], 0);
                p = newline + 1;
            }
        }
    }
    if (stream[0] >= 0 || stream[1] >= 0)
        return G_SOURCE_CONTINUE;
    int status = 0;
    pid_t reap = waitpid(child, &status, WNOHANG);
    if (!reap || (reap < 0 && errno == EINTR))
        return G_SOURCE_CONTINUE;
    child = 0;
    for (int i = 0; i < 2; i++)
        g_string_free(partial[i], TRUE);
    on_done(reap > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return G_SOURCE_REMOVE;
}

int dsh_start(const char *directory, const char *session, const char *prompt, DshLine line, DshDone done) {
    if (child)
        return EBUSY;
    if (strlen(prompt) > 24000)
        return E2BIG;
    char *args[12] = {"/bin/dsh", "headless", "--patch", "/lib/dsh-gui/control.yml", "--json"};
    int a = 5;
    if (*session) {
        args[a++] = "--session-id";
        args[a++] = (char *)session;
    }
    args[a++] = "--";
    args[a++] = (char *)prompt;
    if (!strcmp(prompt, "-"))
        return EINVAL; /* dsh reserves this for stdin. */
    int null = open("/dev/null", O_RDONLY);
    if (null < 0)
        return errno;
    int pipes[2][2] = {{-1, -1}, {-1, -1}};
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        close(null);
        return error;
    }
    error = posix_spawn_file_actions_addchdir_np(&actions, directory);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, null, 0);
    for (int i = 0; i < 2 && !error; i++) {
        if (pipe(pipes[i]) < 0 || fcntl(pipes[i][0], F_SETFL, O_NONBLOCK) < 0)
            error = errno;
        else
            error = posix_spawn_file_actions_adddup2(&actions, pipes[i][1], i + 1);
    }
    if (!error)
        error = posix_spawn(&child, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(null);
    for (int i = 0; i < 2; i++) {
        if (pipes[i][1] >= 0)
            close(pipes[i][1]);
        if (error && pipes[i][0] >= 0)
            close(pipes[i][0]);
        stream[i] = pipes[i][0];
    }
    if (error) {
        child = 0;
        return error;
    }
    for (int i = 0; i < 2; i++)
        partial[i] = g_string_new(NULL);
    on_line = line;
    on_done = done;
    g_timeout_add(55, pump, NULL);
    return 0;
}
