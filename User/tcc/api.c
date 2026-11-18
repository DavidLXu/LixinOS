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

int lx_tcp_connect(const char *ipv4, int port) {
    if (!ipv4 || port <= 0 || port > 65535) return -1;
    unsigned i = 0;
    while (ipv4[i] && i < 15) { MAIL[i] = ipv4[i]; ++i; }
    if (ipv4[i]) return -1;
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
