#define _GNU_SOURCE
// Exercises TLS, Linux clone, futex wake and clear_child_tid on pthread_join.
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;
static char worker_proc_path[48];
static _Thread_local int local;
static int replace_instruction(void) {
    uint32_t *code = mmap(NULL, 4096, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (code == MAP_FAILED)
        return 0;
    int (*run)(void) = (int (*)(void))code;
    code[1] = 0xd65f03c0; // ret
    for (unsigned answer = 1; answer <= 2; ++answer) {
        if (mprotect(code, 4096, PROT_READ | PROT_WRITE))
            return 0;
        code[0] = 0x52800000 | (answer << 5); // mov w0, #answer
        if (mprotect(code, 4096, PROT_READ | PROT_EXEC) || run() != (int)answer)
            return 0;
    }
    return !munmap(code, 4096);
}

static int split_event_buffers(unsigned char *boundary) {
    int counter = eventfd(1, EFD_NONBLOCK);
    int queue = epoll_create1(0);
    struct epoll_event *event = (void *)(boundary - 8);
    event->events = EPOLLIN | EPOLLONESHOT;
    event->data.u64 = 0x13572468;
    if (counter < 0 || queue < 0 || epoll_ctl(queue, EPOLL_CTL_ADD, counter, event))
        return 0;
    memset(event, 0, sizeof *event);
    if (epoll_wait(queue, event, 1, 0) != 1 || event->events != EPOLLIN ||
        event->data.u64 != 0x13572468 || epoll_wait(queue, event, 1, 0))
        return 0;
    struct pollfd *fds = (void *)(boundary - sizeof *fds);
    fds[0] = (struct pollfd){.fd = -1};
    fds[1] = (struct pollfd){.fd = counter, .events = POLLIN};
    return poll(fds, 2, 0) == 1 && !fds[0].revents && fds[1].revents == POLLIN &&
           !close(counter) && !close(queue);
}
static void *worker(void *unused) {
    (void)unused;
    local = 27;
    snprintf(worker_proc_path, sizeof worker_proc_path, "self/task/%d/", gettid());
    struct stat st;
    int proc = open("/proc", O_RDONLY | O_DIRECTORY);
    int present = proc >= 0 && !fstatat(proc, worker_proc_path, &st, 0) && S_ISDIR(st.st_mode);
    if (proc >= 0)
        close(proc);
    DIR *tasks = opendir("/proc/self/task");
    char id[16];
    snprintf(id, sizeof id, "%d", gettid());
    int listed = 0;
    if (tasks) {
        struct dirent *ent;
        while ((ent = readdir(tasks)))
            listed |= !strcmp(ent->d_name, id);
        closedir(tasks);
    }
    int policy;
    struct sched_param params;
    int queried = !pthread_getschedparam(pthread_self(), &policy, &params) &&
                  policy == SCHED_OTHER && !params.sched_priority &&
                  sched_getscheduler(getpid()) == SCHED_OTHER &&
                  sched_getparam(getpid(), &params) == 0 && !params.sched_priority &&
                  sched_getscheduler(0x7fffffff) == -1 && errno == ESRCH && !sched_yield();
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    return (void *)(long)(queried && present && listed && local == 27 && gettid() != getpid());
}
int main(void) {
    char thread_name[16];
    if (prctl(PR_SET_NAME, "browser-io-worker") || prctl(PR_GET_NAME, thread_name) ||
        strcmp(thread_name, "browser-io-work"))
        return 11;
    pthread_t thread;
    void *result = NULL;
    local = 15;
    pthread_mutex_lock(&lock);
    if (pthread_create(&thread, NULL, worker, NULL))
        return 1;
    while (!ready)
        if (pthread_cond_wait(&cond, &lock))
            return 2;
    pthread_mutex_unlock(&lock);
    if (pthread_join(thread, &result) || result != (void *)1 || local != 15)
        return 3;
    struct stat st;
    int proc = open("/proc", O_RDONLY | O_DIRECTORY);
    if (proc < 0 || !fstatat(proc, worker_proc_path, &st, 0) || errno != ENOENT || close(proc))
        return 12;
    int cpu = open("/proc/cpuinfo", O_RDONLY);
    char model[256] = {0};
    if (cpu < 0 || read(cpu, model, sizeof model - 1) <= 0 || !strstr(model, "model name") || close(cpu))
        return 13;
    int first = 0, second = 4;
    if (syscall(SYS_futex, &first, FUTEX_WAKE_OP_PRIVATE, 1, 1, &second,
                FUTEX_OP(FUTEX_OP_ADD, 3, FUTEX_OP_CMP_LT, 5)) ||
        second != 7)
        return 4;
    size_t page = 4096;
    unsigned char *area =
        mmap((void *)(1ULL << 35), page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED || (uintptr_t)area < (1ULL << 32))
        return 5;
    for (int i = 0; i < 2; i++) {
        if (mprotect(area + i * page, page, PROT_READ | PROT_WRITE))
            return 6;
        area[i * page] = 1;
    }
    typedef unsigned long long Vec __attribute__((vector_size(16), aligned(1)));
    volatile Vec *crossing = (volatile Vec *)(area + page - 8);
    *crossing = (Vec){0xabc, 0xdef};
    Vec actual = *crossing;
    if (actual[0] != 0xabc || actual[1] != 0xdef)
        return 7;
    int fd = open("/bin/linux-pthread-probe", O_RDONLY);
    unsigned char header[16], *buffer = area + page - 8;
    if (fd < 0 || read(fd, header, sizeof header) != sizeof header ||
        pread(fd, buffer, sizeof header, 0) != sizeof header ||
        memcmp(buffer, header, sizeof header) || close(fd))
        return 8;
    if (!split_event_buffers(area + page) || munmap(area, 2 * page))
        return 9;
    if (!replace_instruction())
        return 10;
    puts("Linux pthread: TLS, condition and join passed");
    return 0;
}
