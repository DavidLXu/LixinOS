#define _GNU_SOURCE
#include "engine.h"
#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pid_t worker_pid;
static int worker_in = -1;
static int stream[2] = {-1, -1};
static GString *partial[2];
static char *worker_dir;
static gboolean worker_ready_flag;
static gboolean turn_active;
static guint pump_source;
static guint pump_interval_ms;
static DshLine on_line;
static DshDone on_done;

static gboolean pump(gpointer unused);

gboolean dsh_busy(void) {
    return turn_active;
}

gboolean dsh_ready(void) {
    return worker_pid > 0 && worker_ready_flag;
}

static void set_pump_rate(guint ms) {
    if (pump_source && pump_interval_ms == ms)
        return;
    if (pump_source) {
        g_source_remove(pump_source);
        pump_source = 0;
    }
    if (ms > 0) {
        pump_interval_ms = ms;
        pump_source = g_timeout_add(ms, pump, NULL);
    } else {
        pump_interval_ms = 0;
    }
}

static int write_all_fd(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (n == 0)
            return EPIPE;
        sent += (size_t)n;
    }
    return 0;
}

static int send_worker_json(JsonObject *obj) {
    if (worker_in < 0)
        return EPIPE;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);
    g_autofree char *encoded = json_to_string(node, FALSE);
    json_node_free(node);
    g_autofree char *line = g_strconcat(encoded, "\n", NULL);
    return write_all_fd(worker_in, line, strlen(line));
}

static void cleanup_worker_state(void) {
    set_pump_rate(0);
    if (worker_in >= 0) {
        close(worker_in);
        worker_in = -1;
    }
    for (int i = 0; i < 2; i++) {
        if (stream[i] >= 0) {
            close(stream[i]);
            stream[i] = -1;
        }
        if (partial[i]) {
            g_string_free(partial[i], TRUE);
            partial[i] = NULL;
        }
    }
    g_clear_pointer(&worker_dir, g_free);
    worker_pid = 0;
    worker_ready_flag = FALSE;
}

static void handle_worker_line(const char *line, gboolean is_stderr) {
    if (!is_stderr && line[0] == '{') {
        if (strstr(line, "\"worker_ready\"")) {
            g_autoptr(JsonParser) p = json_parser_new();
            if (json_parser_load_from_data(p, line, -1, NULL) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
                JsonObject *o = json_node_get_object(json_parser_get_root(p));
                const char *t = json_object_get_string_member_with_default(o, "type", "");
                if (!strcmp(t, "worker_ready")) {
                    worker_ready_flag = TRUE;
                    return;
                }
            }
        }
        if (strstr(line, "\"turn_done\"")) {
            g_autoptr(JsonParser) p = json_parser_new();
            if (json_parser_load_from_data(p, line, -1, NULL) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
                JsonObject *o = json_node_get_object(json_parser_get_root(p));
                const char *t = json_object_get_string_member_with_default(o, "type", "");
                if (!strcmp(t, "turn_done")) {
                    int code = (int)json_object_get_int_member_with_default(o, "exitCode", 0);
                    turn_active = FALSE;
                    set_pump_rate(120);
                    DshDone cb = on_done;
                    if (cb)
                        cb(code);
                    return;
                }
            }
        }
    }
    if (on_line)
        on_line(line, is_stderr);
}

/* Drain a bounded amount each tick so a busy tool cannot starve the window. */
static gboolean pump(gpointer unused) {
    (void)unused;
    char buf[4096];
    for (int i = 0; i < 2; i++) {
        for (int n = 0; stream[i] >= 0 && n < 64; n++) {
            ssize_t count = read(stream[i], buf, sizeof buf);
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            if (count <= 0) {
                if (partial[i] && partial[i]->len) {
                    handle_worker_line(partial[i]->str, i != 0);
                    g_string_truncate(partial[i], 0);
                }
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
                handle_worker_line(partial[i]->str, i != 0);
                g_string_truncate(partial[i], 0);
                p = newline + 1;
            }
        }
    }
    if (stream[0] >= 0 || stream[1] >= 0)
        return G_SOURCE_CONTINUE;
    int status = 0;
    pid_t reap = waitpid(worker_pid, &status, WNOHANG);
    if (!reap || (reap < 0 && errno == EINTR))
        return G_SOURCE_CONTINUE;
    int code = reap > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    gboolean was_active = turn_active;
    turn_active = FALSE;
    DshDone cb = on_done;
    cleanup_worker_state();
    if (was_active && cb)
        cb(code ? code : -1);
    return G_SOURCE_REMOVE;
}

static void stop_worker(gboolean wait_exit) {
    if (worker_pid <= 0)
        return;
    if (worker_in >= 0) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "type", "shutdown");
        send_worker_json(o);
        json_object_unref(o);
        close(worker_in);
        worker_in = -1;
    }
    pid_t pid = worker_pid;
    cleanup_worker_state();
    if (wait_exit) {
        int status = 0;
        for (int i = 0; i < 60; i++) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r != 0)
                return;
            g_usleep(25000);
        }
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    } else {
        int status = 0;
        waitpid(pid, &status, WNOHANG);
    }
}

static int spawn_worker(const char *directory) {
    char *args[] = {"/bin/dsh", "headless", "--patch", "/lib/dsh-gui/control.yml", NULL};
    int in_pipe[2] = {-1, -1};
    int pipes[2][2] = {{-1, -1}, {-1, -1}};
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
        return error;
    error = posix_spawn_file_actions_addchdir_np(&actions, directory);
    if (!error) {
        if (pipe(in_pipe) < 0)
            error = errno;
        else
            error = posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    }
    for (int i = 0; i < 2 && !error; i++) {
        if (pipe(pipes[i]) < 0 || fcntl(pipes[i][0], F_SETFL, O_NONBLOCK) < 0)
            error = errno;
        else
            error = posix_spawn_file_actions_adddup2(&actions, pipes[i][1], i + 1);
    }
    pid_t pid = 0;
    if (!error)
        error = posix_spawn(&pid, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (in_pipe[0] >= 0)
        close(in_pipe[0]);
    for (int i = 0; i < 2; i++) {
        if (pipes[i][1] >= 0)
            close(pipes[i][1]);
    }
    if (error) {
        if (in_pipe[1] >= 0)
            close(in_pipe[1]);
        for (int i = 0; i < 2; i++)
            if (pipes[i][0] >= 0)
                close(pipes[i][0]);
        return error;
    }
    worker_pid = pid;
    worker_in = in_pipe[1];
    worker_dir = g_strdup(directory);
    worker_ready_flag = FALSE;
    for (int i = 0; i < 2; i++) {
        stream[i] = pipes[i][0];
        partial[i] = g_string_new(NULL);
    }
    set_pump_rate(turn_active ? 16 : 120);
    return 0;
}

void dsh_warmup(const char *directory) {
    if (worker_pid > 0 || !directory || !g_file_test(directory, G_FILE_TEST_IS_DIR))
        return;
    spawn_worker(directory);
}

void dsh_reset(void) {
    if (turn_active)
        return;
    stop_worker(FALSE);
}

void dsh_shutdown(void) {
    turn_active = FALSE;
    on_line = NULL;
    on_done = NULL;
    stop_worker(TRUE);
}

int dsh_cancel(void) {
    if (!turn_active || worker_in < 0)
        return EINVAL;
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "type", "cancel");
    int err = send_worker_json(o);
    json_object_unref(o);
    return err;
}

int dsh_start(const char *directory, const char *session, const char *prompt, DshLine line, DshDone done) {
    if (turn_active)
        return EBUSY;
    if (strlen(prompt) > 24000)
        return E2BIG;
    if (!strcmp(prompt, "-"))
        return EINVAL;
    if (worker_pid > 0 && (!worker_dir || strcmp(worker_dir, directory) != 0 || stream[0] < 0))
        stop_worker(TRUE);
    on_line = line;
    on_done = done;
    turn_active = TRUE;
    if (worker_pid <= 0) {
        int error = spawn_worker(directory);
        if (error) {
            turn_active = FALSE;
            return error;
        }
    }
    set_pump_rate(16);
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "type", "turn");
    json_object_set_string_member(o, "prompt", prompt);
    if (session && *session)
        json_object_set_string_member(o, "sessionId", session);
    int error = send_worker_json(o);
    json_object_unref(o);
    if (error) {
        turn_active = FALSE;
        stop_worker(FALSE);
        return error;
    }
    return 0;
}
