#include "../console.h"
#include "lixin.h"

#define MAIL ((char *)0x443d7000)
static int request(const char *path) {
    unsigned n = 0;
    while (n < 513 && path[n]) { MAIL[n] = path[n]; ++n; }
    if (n == 513) return -1;
    MAIL[n] = 0;
    return 0;
}
long lx_stat(const char *path) {
    return request(path) ? -5 : console_call(7, 0);
}
long lx_dir(const char *path, unsigned index, char name[513]) {
    if (request(path)) return -5;
    long result = console_call(7, (long)index + 1);
    if (result >= 0 || result == LX_DIR) {
        unsigned n = 0;
        do { name[n] = MAIL[513+n]; } while (name[n++] && n < 513);
    }
    return result;
}
int lx_open(const char *path) {
    return request(path) ? -1 : console_call(8, 0);
}
int lx_read(int fd, void *buffer, int count) {
    if (count < 0) return -1;
    if (count > 4096) count = 4096;
    long n = console_call(9, ((unsigned long)(unsigned)fd << 32) | (unsigned)count);
    if (n < 0 || n > count) return -1;
    for (long i = 0; i < n; ++i) ((char *)buffer)[i] = MAIL[i];
    return n;
}
int lx_open_write(const char *path,int append) {return request(path) ? -1 : console_call(40,append);}
int lx_poll(int fd,unsigned events,int timeout_ms) {
    if (fd<0 || fd>65535 || events>3 || timeout_ms < -1) return -1;
    return console_call(41,(unsigned long)fd<<48 | (unsigned long)events<<32 | (unsigned)timeout_ms);
}
int lx_write(int fd, const void *data, int count) {
    if (count < 0 || (!data && count)) return -1;
    if (count > 4096) count = 4096;
    for (int i=0; i<count; ++i) MAIL[i]=((const char *)data)[i];
    return console_call(39, (unsigned long)(unsigned)fd << 32 | (unsigned)count);
}
int lx_close(int fd) { return console_call(10, fd); }
void lx_error(const char *s) {
    while (*s) console_call(11, (unsigned char)*s++);
}
int lx_stdout_is_terminal(void) {
    return console_call(24, 0) == 1;
}
int lx_getchar(void) {
    long byte = console_call(0, 0);
    return byte > 255 ? -1 : byte;
}

long lx_agent_exchange(unsigned operation, unsigned length) {
    if (operation > 255 || length > 32768) return -1;
    return console_call(12, ((long)length << 8) | operation);
}

long lx_process_call(unsigned op, unsigned index) {
    return console_call(18, ((unsigned long)index << 32) | op);
}
int lx_spawn_async(const char *path,int argc,char **argv) {
    if (!path || !argv || argc<1 || argc>512) return -1;
    char *args=(char *)0x443d8000;
    for (int i=-1;i<argc;i++) {
        const char *s=i<0 ? path : argv[i];
        if (!s) return -1;
        unsigned n=0;
        while (n<4096 && s[n]) {args[n]=s[n];n++;}
        if (s[n]) return -1;
        args[n]=0;
        if (lx_process_call(i<0 ? 4 : 5,0)<0) return -1;
    }
    args[0]=0;
    return lx_process_call(18,8);
}
int lx_wait(int pid) {
    *(char *)0x443d8000=0;
    return lx_process_call(19,(unsigned)pid);
}
int lx_pid(void) {
    *(char *)0x443d8000=0;
    return lx_process_call(23,0);
}
unsigned long lx_geometry(void) { return console_call(13, 0); }
int lx_key_byte(void) {
    int key = console_call(0, 1);
#ifdef LX_SAVED_RUNTIME
    while (key == 257) {
        lx_agent_yield();
        key = console_call(0, 1);
    }
#endif
    return key;
}
int lx_timed_byte(unsigned ms) { return console_call(14, ms); }

int lx_tcp_connect(const char *host, int port) {
    if (!host || port <= 0 || port > 65535) return -1;
    unsigned i = 0;
    while (i < 253 && host[i]) { MAIL[i] = host[i]; ++i; }
    if (host[i]) return -1;
    MAIL[i] = 0;
    return console_call(25, port);
}
int lx_tcp_read(int fd, void *data, int count) {
    if (count < 0 || (!data && count)) return -1;
    if (count > 4096) count = 4096;
    long n = console_call(26, ((unsigned long)(unsigned)fd << 32) | (unsigned)count);
    if (n > count) return -1;
    for (int i = 0; i < n; ++i) ((char *)data)[i] = MAIL[i];
    return n;
}
int lx_tcp_write(int fd, const void *data, int count) {
    if (count < 0 || (!data && count)) return -1;
    if (count > 4096) count = 4096;
    for (int i = 0; i < count; ++i) MAIL[i] = ((const char *)data)[i];
    return console_call(27, ((unsigned long)(unsigned)fd << 32) | (unsigned)count);
}
int lx_tcp_close(int fd) { return console_call(28, (unsigned long)(unsigned)fd << 32); }

int lx_random(void *buffer, int count) {
    if (count < 0 || (!buffer && count)) return -1;
    unsigned char *p = buffer;
    while (count) {
        int n = count > 4096 ? 4096 : count;
        if (console_call(30, n) != 0) return -1;
        for (int i = 0; i < n; ++i) p[i] = MAIL[i];
        p += n;
        count -= n;
    }
    return 0;
}

unsigned long lx_canvas_size(void) { return console_call(19, 0); }
int lx_canvas_present(const unsigned *pixels, unsigned x, unsigned y,
                      unsigned width, unsigned height) {
    unsigned long *mail = (unsigned long *)0x443d8000UL;
    mail[0] = (unsigned long)pixels;
    mail[1] = x;
    mail[2] = y;
    mail[3] = width;
    mail[4] = height;
    return console_call(20, 0);
}
long lx_canvas_pointer(void) { return console_call(21, 0); }

int lx_save_file(const char *path, const void *data, unsigned length) {
    if (!path || (!data && length) || length > 32768 || request(path))
        return -1;
    char *bytes = (char *)0x443d8000UL;
    const char *source = data;
    for (unsigned i = 0; i < length; ++i)
        bytes[i] = source[i];
    return console_call(22, length);
}

int lx_save_parts(const char *path, unsigned long descriptor) {
    return request(path) ? -1 : console_call(22, descriptor);
}

long lx_console_call(long op, long arg) { return console_call(op, arg); }
