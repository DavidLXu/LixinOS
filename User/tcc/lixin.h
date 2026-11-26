// LixinOS read-only file and directory API. Paths are relative to the shell cwd.
// stat/dir return a byte size, LX_DIR, LX_END (end of listing), or a lower error.
#define LX_END (-1)
#define LX_DIR (-2)
#define LX_MISSING (-3)
long lx_stat(const char *path);
long lx_dir(const char *path, unsigned index, char name[513]); /* zero-based */
int lx_open(const char *path);
int lx_read(int fd, void *buffer, int count); /* zero at EOF */
int lx_open_write(const char *path,int append);
#define LX_READABLE 1
#define LX_WRITABLE 2
int lx_poll(int fd,unsigned events,int timeout_ms);
int lx_write(int fd, const void *buffer, int count);
int lx_close(int fd);
/* Atomically create or replace a regular file, up to 32 KiB. */
int lx_save_file(const char *path, const void *data, unsigned length);
void lx_error(const char *text); /* write to stderr */
int lx_stdout_is_terminal(void);

// Only an agent run session is granted the model exchange.
#define LX_AGENT_MAILBOX ((volatile char *)0x443d8000UL)
#define LX_AGENT_ASK 0
#define LX_AGENT_SUBMIT 1
#define LX_AGENT_ASK_POLICY 2 /* remaining decimal budget, NUL, then prompt */
#define LX_AGENT_RESULT 3     /* last tool's result, tagged by the broker */
#define LX_AGENT_PRINT 4      /* write UTF-8 to the operator's console */
#define LX_AGENT_POLICY 5     /* mode byte, then two decimal settings */
long lx_agent_exchange(unsigned operation, unsigned length);
int lx_agent_yield(void); /* suspend until the submitted tool has returned */

// Saved LXE programs can launch a foreground child by path and wait for it.
// argv includes argv[0]. Returns its exit status (0..255), or -1 on failure.
int lx_spawn(const char *path, int argc, char **argv);

int lx_spawn_flags(const char *path, int argc, char **argv, unsigned stream_flags);
int lx_spawn_async(const char *path,int argc,char **argv);
int lx_wait(int pid);
int lx_pid(void);
unsigned lx_start_flags(void);
long lx_process_call(unsigned op, unsigned index);
unsigned long lx_geometry(void);
int lx_key_byte(void);
int lx_timed_byte(unsigned ms);

// Native IPv4 TCP. Hostnames use QEMU DNS (up to eight seconds), then connect (up to five seconds). Reads/writes report partial
// progress, LX_TCP_AGAIN when not ready, or -1 on error. Read returns 0 at EOF.
#define LX_TCP_AGAIN (-2)
int lx_tcp_connect(const char *host, int port);
int lx_tcp_read(int fd, void *data, int count);
int lx_tcp_peek(int fd, void *data, int count);
int lx_tcp_write(int fd, const void *data, int count);
int lx_tcp_close(int fd);

// Fill a buffer from virtio-rng. Returns -1 if entropy is unavailable.
int lx_random(void *buffer, int count);
