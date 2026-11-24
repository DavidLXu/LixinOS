#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void stop_bus(pid_t bus)
{
    kill(bus, SIGKILL);
    while (waitpid(bus, NULL, 0) < 0 && errno == EINTR) {}
}

static int64_t monotonic_ms(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int read_address(int fd, char *address, size_t capacity)
{
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    size_t used = 0;
    int64_t end = monotonic_ms() + 5000;
    while (used < capacity - 1) {
        int64_t timeout = end - monotonic_ms();
        if (timeout <= 0) return ETIMEDOUT;
        int available = poll(&ready, 1, (int)timeout);
        if (!available) return ETIMEDOUT;
        if (available < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        ssize_t count = read(fd, address + used, capacity - 1 - used);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return errno;
        }
        if (!count) return EPIPE;
        char *line_end = memchr(address + used, '\n', count);
        if (line_end) {
            *line_end = '\0';
            return strncmp(address, "unix:", 5) == 0 ? 0 : EPROTO;
        }
        used += count;
    }
    return EMSGSIZE;
}

static int start_bus(pid_t *bus, const char *name, char *address, size_t capacity)
{
    const char *path = "/bin/dbus-daemon";
    char socket[96];
    snprintf(socket, sizeof(socket), "--address=unix:abstract=lixin-%s-%ld", name, (long)getpid());
    char *args[] = {(char *)path, "--config-file=/lib/dbus/session.conf", "--nofork",
                    "--print-address=1", socket, NULL};
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) return errno;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        if (!error)
            error = posix_spawn(bus, path, &actions, NULL, args, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pipe_fds[1]);
    if (!error) {
        error = read_address(pipe_fds[0], address, capacity);
        if (error) stop_bus(*bus);
    }
    close(pipe_fds[0]);
    return error;
}

int main(int argc, char **argv)
{
    int first = 1;
    int private_system_bus = argc > first && !strcmp(argv[first], "--private-system-bus");
    first += private_system_bus;
    if (argc > first && !strcmp(argv[first], "--")) ++first;
    if (argc <= first) {
        fprintf(stderr, "usage: dbus-run-session [--private-system-bus] [--] COMMAND [ARGUMENTS]\n");
        return 2;
    }

    char address[256];
    pid_t bus, program, system_bus = 0;
    int error = start_bus(&bus, "session", address, sizeof(address));
    if (error) {
        fprintf(stderr, "Could not start the D-Bus session: %s\n", strerror(error));
        return 1;
    }

    if (setenv("DBUS_SESSION_BUS_ADDRESS", address, 1) < 0)
        error = errno;
    if (!error && private_system_bus && !getenv("DBUS_SYSTEM_BUS_ADDRESS")) {
        pid_t spawned;
        error = start_bus(&spawned, "system", address, sizeof(address));
        if (!error) {
            system_bus = spawned;
            if (setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1) < 0)
                error = errno;
        }
    }
    if (!error)
        error = posix_spawnp(&program, argv[first], NULL, NULL, argv + first, environ);

    int status = 0;
    if (!error) {
        pid_t waited;
        do {
            waited = waitpid(program, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) error = errno;
    }
    if (system_bus) stop_bus(system_bus);
    stop_bus(bus);
    if (error) {
        fprintf(stderr, "%s: %s\n", argv[first], strerror(error));
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
