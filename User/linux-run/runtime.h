#pragma once
#include "cpu.h"
#include <elf.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>

#define PAGE 4096u
#define ARENA (768u << 20)
#define MAIN_BASE 0x10000u
#define LD_BASE (192u << 20)
#define BRK_BASE (194u << 20)
#define MMAP_BASE (196u << 20)
#define STACK_BASE (ARENA - (1u << 20))
#define INTERP "/lib/ld-linux-aarch64.so.1"
struct proc_handle;
struct guest_memory;
struct upper_memory;
struct guest_files;
struct guest_events;
struct pid_namespace {
    unsigned depth;
    int init[32];
};
struct guest_process {
    struct pid_namespace pids;
    int net_namespace;
    struct guest_memory *memory;
    struct upper_memory *upper;
    struct guest_files *descriptors;
    struct guest_events *event_table;
    struct user_namespace *userns;
    struct guest_fs *fs;
    int vfork_active;
    u64 sigreturn_pc, cpu_ticks, data_soft, data_hard;
    unsigned nofile_soft, nofile_hard;
    int child_signal_pid, child_signal_status, child_signal_code, term_signal;
    u64 pending_signals;
    int senders[65];
    int pid, ppid, threads, exiting, status;
    struct guest_process *next;
};
extern struct guest_process *guest_process;
long exec_guest(CPU *cpu, const char *path, u64 argv, u64 envp);
void files_on_exec(CPU *cpu);
int memory_create(struct guest_process *process);
int upper_create(struct guest_process *process);
void memory_share(struct guest_process *process);
void upper_share(struct guest_process *process);
u64 *memory_brk(void);
int data_limit_allows(u64 growth);
u64 high_data_bytes(void);
int fs_clone(struct guest_process *child, int share);
void fs_put(struct guest_process *process);
int fs_confined(void);
long fs_chroot(const char *path);
long fs_chdir(const char *path);
int proc_target_fd(struct proc_handle *dir, const char *name);
long proc_entries(struct proc_handle *dir, u64 addr, size_t size);
int fd_occupied(unsigned fd);
int memory_clone(struct guest_process *child);
void memory_destroy(struct guest_process *process);
int upper_clone(struct guest_process *child);
void upper_destroy(struct guest_process *process);
int files_clone(struct guest_process *child);
void files_destroy(struct guest_process *process);
int events_clone(struct guest_process *child);
void events_destroy(struct guest_process *process);
int namespace_pid(const struct pid_namespace *viewer, const struct pid_namespace *owner,
                  int global);
int process_pid(void);
int process_ppid(void);
long process_wait(CPU *cpu);
_Noreturn void process_exit(int code);
extern int tracing, independent_processes;
struct backing {
    int fd;
    unsigned refs;
    int open;
    char *path;
    u64 size, inode, dev;
    struct timespec modified;
    struct backing *next;
};
struct backing *open_backing(const char *path);
void close_backing(struct backing *file);
int set_mapping(u64 start, u64 length, int prot, struct backing *file, u64 offset, u64 file_bytes);
long discard_pages(u64 start, u64 length);
void allow_program_path(const char *absolute, const char *executable);

void fail(const char *why);
void mem_init(void);
void files_init(void);
void events_init(void);
void upper_init(void);
void *guest_ptr(u64 addr, u64 length, int access);
int guest_copy(u64 addr, void *value, size_t size, int writing);
int page_update(u64 start, u64 end, int prot, int require_mapped);
u64 map_linux(u64 addr, u64 length, unsigned prot, unsigned flags, struct backing *file, u64 off);
int read_segment(int fd, void *out, size_t len, u64 offset);
long service(CPU *cpu);

int state_entry(const char *name, const char *directory, char out[513]);
int state_path(const char *path, char out[513]);
void state_init(const char *path, const char *home);
int state_parent(const char *path);

CPU *thread_cpu(void);
CPU *start_threads(void);
void thread_pause(void);
long thread_futex_wait(CPU *cpu, int *word, int expected, int op, const struct timespec *timeout);
int thread_futex_wake(int *word, int count);
void thread_resume(CPU *cpu);
void interpret(CPU *cpu);
long clone_thread(CPU *cpu);
int thread_tid(void);
// IDs in a procfs handle stay bound to the selected process across fork.
struct proc_task {
    int owner, tid;
    unsigned depth;
};
int process_for_proc(int pid);
int process_thread_count(int owner);
int process_next_tid(struct proc_task task, int after);
int process_thread_status(struct proc_task task, char *buf, size_t size);
long thread_sched_query(int tid, int policy, u64 param);
struct timespec;
void thread_cputime(int process, struct timespec *out);
long thread_set_tid(u64 addr);
long thread_mask(int how, u64 set, u64 old, u64 size);
_Noreturn void thread_exit(int code);

long high_map(u64 addr, u64 length, unsigned prot, unsigned flags);
long high_update(u64 addr, u64 length, int prot);
void *high_ptr(u64 addr, u64 len, int access);

#define GUEST_FDS 256
void reserve_descriptors(unsigned used);
int host_descriptor(u64 fd);
int free_guest_fd(void);
int event_fd(int fd);
int channel_stat_mode(int fd);
u64 event_key(unsigned fd);
void event_set_key(unsigned fd, u64 key);
void forget_event_key(u64 key);
int held_event_key(u64 key);
void channel_duplicate(int src, int dst, int cloexec);
int event_ready(int fd, short requested);
long event_io(CPU *cpu, int fd, u64 addr, size_t length, int writing);
long event_service(CPU *cpu);
void event_closed(unsigned fd);

void thread_slice(CPU *cpu);
long event_fcntl(int fd, int cmd, long arg);
long adopt_fd(int fd);
long message_service(CPU *cpu);
long channel_pair(CPU *cpu);
long channel_sockopt(CPU *cpu);
long channel_message(CPU *cpu, int fd, u64 addr, int writing, int flags);
long channel_shutdown(int fd, u64 how);
long channel_io(CPU *cpu, int fd, u64 addr, u64 size, int writing, int flags);
long high_file(u64 hint, u64 bytes, unsigned prot, unsigned flags, int fd, u64 offset);

int state_resource(const char *path, char out[513]);

int socket_family(u64 guestfd);
long socket_chunk(int fd, u64 addr, u64 bytes, int sending, int flags);

struct proc_handle;
struct stat;
unsigned namespace_id(int group);
int namespace_clone(struct guest_process *child, int create);
void namespace_put(struct user_namespace *ns);
long namespace_unshare(u64 flags);
void thread_new_namespace(void);
void thread_exec_name(const char *path);
long thread_caps(CPU *cpu);
struct linux_sigaction {
    u64 handler, flags, restorer, mask;
};
long signal_process(int pid, int signo);
int signal_supported(int signo);
struct linux_sigaction *process_action(struct guest_process *process, int signo);
struct linux_sigaction *child_action(struct guest_process *process);
void signal_checkpoint(CPU *cpu);
void signal_trampoline(void);
long signal_return(CPU *cpu);
int signal_ready(void);
long masked_poll(CPU *cpu);
int proc_node(const char *path, struct proc_handle *dir, struct proc_task *task);
int proc_open(int node, struct proc_task task, int flags, struct proc_handle **out);
void proc_inherit(struct proc_handle *h);
void proc_close(struct proc_handle *h);
void proc_fstat(struct proc_handle *h, struct stat *st);
int proc_flags(struct proc_handle *h);
void proc_stat(int node, struct proc_task task, struct stat *st);
long proc_io(struct proc_handle *h, u64 addr, size_t len, int writing);
unsigned thread_effective_caps(void);

struct shm_handle;
int shm_path(const char *path);
int shm_open_file(const char *path, unsigned flags, unsigned mode, struct shm_handle **out);
void shm_retain(struct shm_handle *h);
void shm_drop(struct shm_handle *h);
int shm_remove(const char *path);
long shm_stat_name(const char *path, struct stat *st);
long shm_stat_handle(struct shm_handle *h, struct stat *st);
int shm_raw(struct shm_handle *h);
int shm_flags(struct shm_handle *h);
long shm_io(struct shm_handle *h, u64 addr, size_t len, int writing, off_t *at);
long shm_seek(struct shm_handle *h, off_t distance, int whence);
int export_native_fd(unsigned fd);

struct held_fd;
struct held_channel;
long hold_fd(unsigned fd, unsigned transport, struct held_fd **out);
void drop_fd(struct held_fd *h);
long receive_fd(const struct held_fd *h, int cloexec);
long hold_channel(int fd, int transport, struct held_channel **out);
void drop_channel(struct held_channel *h);
void receive_channel(const struct held_channel *h, int fd, int cloexec);
