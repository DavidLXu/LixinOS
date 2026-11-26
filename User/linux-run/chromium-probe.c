#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exercise both ends of a loadable segment larger than the initial PIE slot
 * without putting the zero-filled segment in the executable on disk. */
static volatile unsigned char bss[200u << 20];

static int map_write(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    int err = errno;
    close(fd);
    errno = err;
    return n == (ssize_t)strlen(text) ? 0 : -1;
}
static int packet_credentials(void) {
    int pair[2], enabled = 1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) ||
        setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof enabled))
        return 0;
    pid_t child = fork();
    if (child < 0)
        return 0;
    if (!child) {
        struct iovec parts[] = {{"he", 2}, {"llo", 3}};
        struct msghdr message = {.msg_iov = parts, .msg_iovlen = 2};
        _exit(sendmsg(pair[1], &message, 0) == 5 ? 0 : 1);
    }
    int status;
    if (waitpid(child, &status, 0) != child || status)
        return 0;
    close(pair[1]);
    char data[5], ancillary[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec parts[] = {{data, 1}, {data + 1, sizeof data - 1}};
    struct msghdr message = {.msg_iov = parts, .msg_iovlen = 2};
    if (recvmsg(pair[0], &message, MSG_PEEK) != 5 || !(message.msg_flags & MSG_CTRUNC))
        return 0;
    message.msg_control = ancillary;
    message.msg_controllen = sizeof ancillary;
    if (recvmsg(pair[0], &message, 0) != 5 || message.msg_flags || memcmp(data, "hello", 5))
        return 0;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_CREDENTIALS ||
        header->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
        return 0;
    struct ucred identity;
    memcpy(&identity, CMSG_DATA(header), sizeof identity);
    close(pair[0]);
    return identity.pid == child && identity.uid == getuid() && identity.gid == getgid();
}

static unsigned char helper_stack[32768] __attribute__((aligned(16)));
static int restrict_root(void *arg) {
    (void)arg;
    if (chroot("/proc/self/fdinfo/") || chdir("/"))
        return 1;
    bss[0] = 89;
    return 0;
}
static int filesystem_probe(void) {
    int proc = open("/proc", O_RDONLY | O_DIRECTORY);
    pid_t helper = clone(restrict_root, helper_stack + sizeof helper_stack,
                         CLONE_VM | CLONE_VFORK | CLONE_FS | SIGCHLD, NULL);
    siginfo_t info = {0};
    if (proc < 0 || helper <= 1 || waitid(P_PID, helper, &info, WEXITED | WNOWAIT) ||
        info.si_pid != helper || info.si_status || waitid(P_ALL, 0, &info, WEXITED) ||
        bss[0] != 89 || !access("/proc", F_OK) || open("/dev/null", O_RDWR) != -1 ||
        openat(proc, "/proc/self/fd", O_DIRECTORY) != -1)
        return 0;
    int saved = openat(proc, "self/fd", O_RDONLY | O_DIRECTORY);
    char entries[256];
    return saved >= 0 && syscall(SYS_getdents64, saved, entries, sizeof entries) > 0 &&
           !close(saved) && !close(proc);
}

static int shared_file_probe(int type) {
    const char *name = "/dev/shm/window-probe";
    int pair[2];
    if (socketpair(AF_UNIX, type, 0, pair))
        return 0;
    int writer = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    int reader = open(name, O_RDONLY);
    if (writer < 0 || reader < 0 || open(name, O_EXCL | O_CREAT | O_RDWR, 0600) != -1 ||
        errno != EEXIST || unlink(name) || open(name, O_RDONLY) != -1 || ftruncate(writer, 4096))
        return 0;
    char *view = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, writer, 0);
    if (view == MAP_FAILED)
        return 0;
    *view = 91;
    char byte = 'a';
    struct iovec iov = {&byte, 1};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl = {0};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf, .msg_controllen = sizeof ctl.buf};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &reader, sizeof reader);
    if (type == SOCK_STREAM && write(pair[1], "p", 1) != 1)
        return 0;
    if (sendmsg(pair[1], &msg, 0) != 1 || close(reader) || close(writer))
        return 0;
    pid_t child = fork();
    if (!child) {
        if (type == SOCK_STREAM) {
            if (read(pair[0], &byte, 1) != 1 || byte != 'p' ||
                recvmsg(pair[0], &msg, MSG_PEEK) != 1 || byte != 'a')
                _exit(4);
            cm = CMSG_FIRSTHDR(&msg);
            if (!cm || cm->cmsg_type != SCM_RIGHTS)
                _exit(5);
            int peeked;
            memcpy(&peeked, CMSG_DATA(cm), sizeof peeked);
            if (close(peeked))
                _exit(6);
            msg.msg_controllen = sizeof ctl.buf;
        }
        if (recvmsg(pair[0], &msg, MSG_CMSG_CLOEXEC) != 1 || msg.msg_flags)
            _exit(1);
        cm = CMSG_FIRSTHDR(&msg);
        if (!cm || cm->cmsg_type != SCM_RIGHTS)
            _exit(2);
        int fd;
        memcpy(&fd, CMSG_DATA(cm), sizeof fd);
        char *shared = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        if (shared == MAP_FAILED || *shared != 91 || fcntl(fd, F_GETFD) != FD_CLOEXEC ||
            fcntl(fd, F_GETFL) != O_RDONLY || write(fd, "b", 1) != -1 || errno != EBADF ||
            mprotect(shared, 4096, PROT_READ | PROT_WRITE) != -1 || errno != EACCES)
            _exit(3);
        *view = 19;
        _exit(0);
    }
    int status;
    int ok = child > 0 && waitpid(child, &status, 0) == child && !status && *view == 19;
    close(pair[0]);
    close(pair[1]);
    munmap(view, 4096);
    return ok;
}

static int data_limit_probe(void) {
    // PartitionAlloc probes whether an address is mapped using the kernel copy-out.
    if (syscall(SYS_prlimit64, 0, RLIMIT_NPROC, NULL, (void *)0x1000) != -1 || errno != EFAULT)
        return 0;
    pid_t child = fork();
    if (!child) {
        char *reservation = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct rlimit limit = {0, 0}, original;
        if (reservation == MAP_FAILED || prlimit(0, RLIMIT_DATA, &limit, &original) ||
            original.rlim_cur != RLIM_INFINITY || setrlimit(RLIMIT_DATA, &original) != -1 ||
            errno != EPERM)
            _exit(1);
        if (mmap(NULL, 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
            errno != ENOMEM || mprotect(reservation, 4096, PROT_WRITE) != -1 || errno != ENOMEM ||
            mmap((void *)(1ULL << 35), 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
                MAP_FAILED ||
            errno != ENOMEM)
            _exit(2);
        _exit(0);
    }
    int status;
    struct rlimit original;
    return child > 0 && waitpid(child, &status, 0) == child && !status &&
           !getrlimit(RLIMIT_DATA, &original) && original.rlim_max == RLIM_INFINITY;
}

static void *park_worker(void *arg) {
    int word = 0;
    if (write(*(int *)arg, "w", 1) == 1)
        syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    return NULL;
}
static int parked_group_exit(void) {
    int ready[2];
    if (pipe(ready))
        return 0;
    pid_t child = fork();
    if (!child) {
        pthread_t parked;
        char started;
        if (pthread_create(&parked, NULL, park_worker, &ready[1]) ||
            read(ready[0], &started, 1) != 1)
            _exit(2);
        usleep(20000);
        _exit(31);
    }
    close(ready[0]);
    close(ready[1]);
    int status;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 31;
}
static int termination_probe(void) {
    int fds[2];
    if (pipe(fds))
        return 0;
    pid_t child = fork();
    if (!child) {
        char byte;
        close(fds[1]);
        read(fds[0], &byte, 1);
        _exit(1);
    }
    int status;
    int ok = child > 0 && !kill(child, SIGKILL) && waitpid(child, &status, 0) == child &&
             WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    close(fds[0]);
    close(fds[1]);
    return ok;
}

static int epoll_probe(void) {
    int counter = eventfd(0, EFD_NONBLOCK), ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 906};
    if (counter < 0 || ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, counter, &ev))
        return 0;
    pid_t child = fork();
    if (!child) {
        int copy = fcntl(counter, F_DUPFD_CLOEXEC, 40);
        if (copy < 40 || close(counter) || eventfd(0, EFD_NONBLOCK) != counter)
            _exit(1);
        if (eventfd_write(copy, 12) || epoll_wait(ep, &ev, 1, 0) != 1 || ev.data.u64 != 906 ||
            ev.events != EPOLLIN)
            _exit(2);
        _exit(0);
    }
    int status;
    eventfd_t value;
    int ok = child > 0 && waitpid(child, &status, 0) == child && !status &&
             epoll_wait(ep, &ev, 1, 0) == 1 && !eventfd_read(counter, &value) && value == 12 &&
             !epoll_wait(ep, &ev, 1, 0);
    close(counter);
    close(ep);
    return ok;
}

static int mapping_capacity(void) {
    enum { COUNT = 128 };
    unsigned char *views[COUNT];
    for (int i = 0; i < COUNT; ++i) {
        int fd = memfd_create("surface", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, 4096))
            return 1;
        views[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (views[i] == MAP_FAILED || close(fd))
            return 2;
        *views[i] = i;
    }
    /* Leave one isolated hole and one pair of adjacent holes. */
    if (munmap(views[0], 4096) || munmap(views[2], 4096) || munmap(views[3], 4096))
        return 3;
    const size_t span = (2U << 20) + 4096;
    int fd = memfd_create("wide-surface", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, span))
        return 4;
    unsigned char *wide = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (wide == MAP_FAILED || close(fd))
        return 5;
    wide[0] = 42;
    wide[span - 1] = 99;
    for (int i = 1; i < COUNT; ++i)
        if (i != 2 && i != 3 && (*views[i] != i || munmap(views[i], 4096)))
            return 6;
    if (wide[0] != 42 || wide[span - 1] != 99 || munmap(wide, span))
        return 7;
    puts("128 independent shared mappings and fragmented multi-block allocation passed");
    return 0;
}

static int descriptor_capacity(void) {
    int copies[72];
    const char *path = "/tmp/fd-capacity";
    int file = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (file < 0 || write(file, "abc", 3) != 3 || lseek(file, 0, SEEK_SET))
        return 1;
    for (size_t i = 0; i < sizeof copies / sizeof *copies; ++i)
        if ((copies[i] = open(path, O_RDONLY)) < 0)
            return 2;
    int alias = fcntl(file, F_DUPFD_CLOEXEC, 90);
    if (alias < 90 || fcntl(alias, F_GETFD) != FD_CLOEXEC || fcntl(file, F_GETFD))
        return 3;
    struct rlimit original, reduced;
    if (getrlimit(RLIMIT_NOFILE, &original))
        return 4;
    reduced = original;
    reduced.rlim_cur = 4;
    if (setrlimit(RLIMIT_NOFILE, &reduced))
        return 4;
    pid_t child = fork();
    char byte;
    if (!child) {
        struct rlimit inherited;
        int limited = !getrlimit(RLIMIT_NOFILE, &inherited) && inherited.rlim_cur == 4 &&
                      open(path, O_RDONLY) == -1 && errno == EMFILE &&
                      fcntl(file, F_DUPFD, 4) == -1 && errno == EINVAL &&
                      !close(0) && open("/dev/null", O_RDONLY) == 0 && read(0, &byte, 1) == 0;
        _exit(limited && read(alias, &byte, 1) == 1 && byte == 'a' ? 0 : 4);
    }
    if (setrlimit(RLIMIT_NOFILE, &original))
        return 4;
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || status || read(file, &byte, 1) != 1 ||
        byte != 'b')
        return 5;
    for (size_t i = 0; i < sizeof copies / sizeof *copies; ++i)
        if (read(copies[i], &byte, 1) != 1 || byte != 'a' || close(copies[i]))
            return 6;
    char resolved[4096];
    if (!realpath(path, resolved) || strcmp(resolved, path) ||
        readlink(path, resolved, sizeof resolved) != -1 || errno != EINVAL)
        return 8;
    int dir = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (close(file) || close(alias) || dir < 0 ||
        renameat(dir, "fd-capacity", AT_FDCWD, "/tmp/fd-renamed") ||
        rename("/tmp/fd-renamed", "/lib/linux-run/escaped") != -1 || errno != EACCES ||
        renameat(AT_FDCWD, "/tmp/fd-renamed", dir, "fd-capacity") || close(dir) || unlink(path))
        return 7;
    puts("72 independent file descriptions and inherited shared offset passed");
    return 0;
}

// Read the same nested namespace leader through its own and its parent's procfs view.
static int check_task_status(pid_t pid, int nested) {
    char path[64], expected[64], status[512] = {0};
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    int dir = open(path, O_RDONLY | O_DIRECTORY);
    if (dir < 0)
        return 0;
    snprintf(path, sizeof path, "%d/status", pid);
    int fd = openat(dir, path, O_RDONLY);
    close(dir);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, status, sizeof status - 1);
    close(fd);
    snprintf(expected, sizeof expected, "NSpid:\t%d%s\n", pid, nested ? "\t1" : "");
    return n > 0 && strstr(status, expected);
}
static int namespace_probe(void) {
    int sync[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sync))
        return 0;
    pid_t child =
        syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (child < 0)
        return 0;
    if (!child) {
        char ack;
        close(sync[0]);
        if (read(sync[1], &ack, 1) != 1 || !check_task_status(1, 0))
            _exit(1);
        close(sync[1]);
        if (getpid() != 1 || getppid() != 0 || syscall(SYS_gettid) != 1 ||
            socket(AF_INET, SOCK_STREAM, 0) != -1 || errno != EAFNOSUPPORT || getuid() != 65534 ||
            getgid() != 65534 || map_write("/proc/self/uid_map", "0 0 1") != -1 || errno != EPERM ||
            map_write("/proc/self/gid_map", "1 1000 1") != -1 || errno != EPERM ||
            map_write("/proc/self/setgroups", "deny") ||
            map_write("/proc/self/gid_map", "1 1000 1") ||
            map_write("/proc/self/uid_map", "0 1000 1") || getuid() != 0 || getgid() != 1 ||
            map_write("/proc/self/uid_map", "2 1000 1") != -1 || errno != EPERM ||
            prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
            prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) != -1 || !filesystem_probe())
            _exit(1);
        _exit(0);
    }
    close(sync[1]);
    int visible = check_task_status(child, 1);
    if (write(sync[0], "x", 1) != 1)
        visible = 0;
    close(sync[0]);
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status) &&
           visible && !check_task_status(child, 1) &&
           getuid() == 1000 && getgid() == 1000 && prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0;
}

static volatile sig_atomic_t notified, child_code;
static void child_exited(int signo, siginfo_t *info, void *context) {
    if (signo == SIGCHLD && info->si_code == CLD_EXITED && context) {
        notified = info->si_pid;
        child_code = info->si_status;
    }
}

static int exec_probe(void) {
    struct sigaction act = {.sa_sigaction = child_exited, .sa_flags = SA_SIGINFO}, previous;
    sigemptyset(&act.sa_mask);
    sigset_t mask, prior;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigaction(SIGCHLD, &act, &previous) || sigprocmask(SIG_BLOCK, &mask, &prior))
        return 0;
    pid_t child = fork();
    if (child < 0)
        return 0;
    if (!child) {
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0 || dup2(null, 0) || dup3(null, 90, O_CLOEXEC) != 90)
            _exit(1);
        bss[0] = 32;
        char *args[] = {"self-test", "exec-child", NULL}, *env[] = {"VALUE=exec", NULL};
        execve("/proc/self/exe", args, env);
        _exit(2);
    }
    struct timespec timeout = {.tv_sec = 3};
    int status;
    if (ppoll(NULL, 0, &timeout, &prior) != -1 || errno != EINTR || notified != child ||
        child_code != 43 || waitpid(child, &status, 0) != child || status != (43 << 8))
        return 0;
    sigset_t after;
    return !sigprocmask(SIG_SETMASK, &prior, &after) && sigismember(&after, SIGCHLD) &&
           !sigaction(SIGCHLD, &previous, NULL);
}

static int range_op(int fd, int cmd, short type, off_t start, off_t len) {
    struct flock lock = {.l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len};
    return fcntl(fd, cmd, &lock);
}
static int shared_space_probe(void) {
    struct statfs before, after;
    const char *name = "/dev/shm/space-probe";
    if (statfs("/dev/shm", &before) || before.f_type != 0x01021994 ||
        before.f_bsize != 4096 || !before.f_bavail || before.f_bavail > before.f_blocks)
        return 1;
    int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || ftruncate(fd, 2 << 20) || fstatfs(fd, &after) ||
        after.f_blocks != before.f_blocks ||
        after.f_bavail + (2 << 20) / 4096 > before.f_bavail ||
        unlink(name) || close(fd) || statfs(name, &after) != -1 || errno != ENOENT)
        return 2;
    puts("Shared-memory capacity and allocated blocks passed");
    return 0;
}

static int record_lock_probe(void) {
    int fd = open("/tmp/lock-probe", O_RDWR | O_CREAT, 0600);
    int notify[2];
    int duplicate = dup(fd);
    off_t base = 1L << 30; /* SQLite's pending lock byte. */
    if (fd < 0 || duplicate < 0 || pipe(notify) ||
        range_op(fd, F_SETLK, F_RDLCK, base, 8) ||
        range_op(fd, F_SETLK, F_UNLCK, base + 2, 4))
        return 1;
    pid_t parent = getpid();
    pid_t child = fork();
    if (!child) {
        close(notify[0]);
        struct flock conflict = {.l_type=F_WRLCK, .l_whence=SEEK_SET, .l_start=base, .l_len=1};
        if (fcntl(fd, F_GETLK, &conflict) || conflict.l_type != F_RDLCK ||
            conflict.l_start != base || conflict.l_len != 2 || conflict.l_pid != parent ||
            range_op(fd, F_SETLK, F_RDLCK, base, 2) ||
            range_op(fd, F_SETLK, F_WRLCK, base + 2, 4) ||
            range_op(fd, F_SETLK, F_WRLCK, base + 6, 1) != -1 || errno != EAGAIN ||
            write(notify[1], "r", 1) != 1) _exit(2);
        _exit(range_op(fd, F_SETLKW, F_WRLCK, base, 0) ? 3 : 0);
    }
    close(notify[1]);
    int status;
    char byte;
    if (child < 0 || read(notify[0], &byte, 1) != 1 || close(duplicate) ||
        waitpid(child, &status, 0) != child || status ||
        range_op(fd, F_SETLK, F_WRLCK, base + 8, -8) || close(fd))
        return 4;
    puts("SQLite byte ranges, shared readers, conflicts, blocking and close passed");
    return 0;
}

static int tcp_peek_probe(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(port)};
    char data[8];
    if (fd < 0 || !inet_aton("10.0.2.2", &peer.sin_addr) ||
        connect(fd, (const struct sockaddr *)&peer, sizeof peer) ||
        recv(fd, data, 1, MSG_PEEK | MSG_DONTWAIT) != -1 || errno != EAGAIN ||
        send(fd, "g", 1, 0) != 1)
        return 1;
    for (int i = 0; i < 2; ++i)
        if (recv(fd, data, 1, MSG_PEEK) != 1 || data[0] != 'A')
            return 2;
    if (read(fd, data, 1) != 1 || data[0] != 'A' ||
        recv(fd, data, 1, MSG_PEEK) != 1 || data[0] != 'B' ||
        recv(fd, data, 1, 0) != 1 || data[0] != 'B' ||
        recv(fd, data, 1, MSG_PEEK) != 0 || close(fd))
        return 3;
    puts("TCP peek preserved bytes, EAGAIN and EOF");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "tcp-peek"))
        return tcp_peek_probe(atoi(argv[2]));
    if (argc == 2 && !strcmp(argv[1], "shared-space"))
        return shared_space_probe();
    if (argc == 2 && !strcmp(argv[1], "record-locks"))
        return record_lock_probe();
    if (argc == 2 && !strcmp(argv[1], "shared-mappings"))
        return mapping_capacity();
    if (argc == 2 && !strcmp(argv[1], "descriptors"))
        return descriptor_capacity();
    if (argc == 2 && !strcmp(argv[1], "exec-child")) {
        char byte;
        const char *value = getenv("VALUE");
        return !strcmp(argv[0], "self-test") && value && !strcmp(value, "exec") && !bss[0] &&
                       !read(0, &byte, 1) && write(0, "x", 1) == 1 && fcntl(90, F_GETFD) == -1 &&
                       errno == EBADF
                   ? 43
                   : 3;
    }
    if (!argc)
        return 1;
    uid_t ru, eu, su;
    gid_t rg, eg, sg;
    if (getresuid(&ru, &eu, &su) || getresgid(&rg, &eg, &sg) || ru != getuid() || eu != geteuid() ||
        su != eu || rg != getgid() || eg != getegid() || sg != eg)
        return 1;
    struct sigaction ignore = {.sa_handler = SIG_IGN}, old, current;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old) || old.sa_handler != SIG_DFL ||
        sigaction(SIGPIPE, NULL, &current) || current.sa_handler != SIG_IGN ||
        sigaction(SIGPIPE, &old, NULL))
        return 1;
    if (bss[0] || bss[sizeof(bss) - 1])
        return 1;
    bss[0] = 17;
    bss[sizeof(bss) - 1] = 25;
    if (bss[0] + bss[sizeof(bss) - 1] != 42)
        return 1;
    int fd = open(argv[0], O_RDONLY);
    if (fd < 0)
        return 2;
    unsigned char *page = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (page == MAP_FAILED || page[0] != 0x7f ||
        mprotect(page, 4096, PROT_READ | PROT_WRITE) != -1 || errno != EACCES || munmap(page, 4096))
        return 2;
    int pair[2];
    char byte;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) || shutdown(pair[0], SHUT_RD) ||
        shutdown(pair[1], SHUT_WR) || send(pair[1], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE ||
        recv(pair[0], &byte, 1, 0) != 0 || send(pair[0], "a", 1, 0) != 1 ||
        recv(pair[1], &byte, 1, 0) != 1 || byte != 'a')
        return 3;
    close(pair[0]);
    close(pair[1]);
    unsigned char *low = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (low == MAP_FAILED || (uintptr_t)low >= (1ULL << 32))
        return 4;
    low[0] = low[4096] = low[8191] = low[8192] = 41;
    if (madvise(low + 4096, 1, MADV_DONTNEED) || low[4096] || low[8191] ||
        low[0] != 41 || low[8192] != 41 || munmap(low, 3 * 4096))
        return 4;
    volatile unsigned char *high = mmap((void *)(1ULL << 36), 4096, PROT_READ | PROT_WRITE,
                                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (high == MAP_FAILED || (unsigned long)high < (1ULL << 32))
        return 4;
    *high = 41;
    if (pipe(pair))
        return 4;
    const pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
        return 4;
    if (!child) {
        close(pair[0]);
        if (*high != 41)
            _exit(3);
        *high = 42;
        bss[0] = 99;
        if (getppid() != parent || getpid() == parent || write(pair[1], "p", 1) != 1)
            _exit(2);
        _exit(37);
    }
    close(pair[1]);
    int status;
    if (read(pair[0], &byte, 1) != 1 || byte != 'p' || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 37 || bss[0] != 17 || *high != 41 ||
        read(pair[0], &byte, 1) != 0 || waitpid(child, &status, WNOHANG) != -1 || errno != ECHILD)
        return 4;
    close(pair[0]);
    if (!namespace_probe())
        return 5;
    if (!packet_credentials())
        return 6;
    if (!exec_probe())
        return 7;
    if (!epoll_probe() || !data_limit_probe() || !termination_probe() || !parked_group_exit())
        return 9;
    if (!shared_file_probe(SOCK_SEQPACKET) || !shared_file_probe(SOCK_STREAM))
        return 8;
    struct timespec begin, end, process;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &begin) ||
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end) ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process) ||
        (end.tv_sec == begin.tv_sec && end.tv_nsec <= begin.tv_nsec) ||
        (process.tv_sec == end.tv_sec && process.tv_nsec < end.tv_nsec))
        return 9;
    puts("Chromium startup primitives passed");
    return 0;
}
