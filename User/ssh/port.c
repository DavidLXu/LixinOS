#include "../console.h"
#include "../tcc/lixin.h"
#include <stdint.h>
#include <string.h>

#define MAIL ((void *)0x443d7000)
#define DATA ((void *)0x443d8000)
extern char __data_image[], __data_begin[], __data_end[];
extern int ssh_main(const unsigned char *args, size_t len);

// The protocol runner needs more stack than the region above the argument mailbox.
// The kernel zeroes the complete writable region on every utility launch.
asm(".section .text.start,\"ax\"\n.global _start\n_start:\n"
    "ldr x3, =0x443d7000\nmov sp, x3\nb ssh_start\n.previous");
void ssh_start(const char *cwd, const unsigned char *args, size_t len) {
    (void)cwd;
    memcpy(__data_begin, __data_image, __data_end - __data_begin);
    if (console_call(24, 1) != 1) {
        lx_error("ssh requires an interactive terminal\n");
        finish(1);
    }
    finish(ssh_main(args, len));
}
int ssh_key(unsigned ms) { return console_call(14, ms); }
void ssh_output(const unsigned char *data, size_t len) {
    while (len--) console_call(1, 256 + *data++);
}
void ssh_abort(void) { lx_error("ssh: client aborted\n"); finish(1); }
int ssh_pin(const char *path, const char *fingerprint, size_t len, int create) {
    // Called with a canonical IPv4 address and port in a fixed-size local path.
    long size = lx_stat(path);
    if (size == -3) {
        if (!create) return 0;
        strcpy(MAIL, path);
        memcpy(DATA, fingerprint, len);
        if (!console_call(31, len)) return 1;
        size = lx_stat(path); // Another pane may have created the same record.
    }
    if (size != (long)len || len > 64) return -1;
    char known[64];
    int fd = lx_open(path);
    if (fd < 0) return -1;
    int n = lx_read(fd, known, len);
    lx_close(fd);
    return n == (int)len && !memcmp(known, fingerprint, len) ? 1 : -1;
}

int ssh_file(const char *path, unsigned char *data, size_t capacity) {
    long size = lx_stat(path);
    if (size < 0) return (int)size;
    if ((size_t)size > capacity) return -1;
    int fd = lx_open(path);
    if (fd < 0) return -1;
    int n = lx_read(fd, data, capacity);
    lx_close(fd);
    explicit_bzero(MAIL, capacity);
    return n == size ? n : -1;
}

int ssh_create_seed(const unsigned char *key) {
    strcpy(MAIL, "/.ssh-gpu20-key");
    memcpy(DATA, key, 32);
    long result = console_call(31, 32);
    explicit_bzero(DATA, 32);
    return result;
}
