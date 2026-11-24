// Verify the staged answer and publish it in the same filesystem revision.
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/sha.h>
#include "../file-time.h"
#include <string.h>
#include "../console.h"
#include "../tcc/lixin.h"

// Encode size, on-volume timestamps, and SHA-256 without relying on C/Rust struct layout.
int lx_codex_version(const char *path, unsigned char version[48]) {
    if (strlen(path) > 512) return ENAMETOOLONG;
    int64_t size = lx_stat(path);
    memset(version, 0, 48);
    memcpy(version, &size, 8);
    if (size == LX_MISSING) return 0;
    if (size == LX_DIR) return EISDIR;
    if (size < 0) return EIO;
    uint32_t times[] = {lx_file_time(0), lx_file_time(1)};
    memcpy(version + 8, times, sizeof(times));
    int fd = lx_open(path);
    if (fd < 0) return EIO;
    SHA256_CTX hash;
    SHA256_Init(&hash);
    char buf[4096];
    int count;
    int64_t total = 0;
    while ((count = lx_read(fd, buf, sizeof(buf))) > 0) {
        total += count;
        SHA256_Update(&hash, buf, count);
    }
    lx_close(fd);
    SHA256_Final(version + 16, &hash);
    return count == 0 && total == size ? 0 : EIO;
}

int lx_codex_publish(const char *from, const char *to, const unsigned char *body, size_t len,
                     const unsigned char expected[48]) {
    char *mail = (char *)0x443d7000UL;
    if (strlen(from) > 512 || strlen(to) > 512) return ENAMETOOLONG;
    if (console_call(45, 0) < 0) return EBUSY;
    unsigned char current[48];
    int error = lx_codex_version(to, current);
    if (!error && memcmp(current, expected, sizeof(current))) error = ESTALE;
    if (!error) {
        int fd = lx_open(from);
        char buf[4096];
        size_t pos = 0;
        if (fd < 0) error = EIO;
        while (!error && pos < len) {
            int count = lx_read(fd, buf, len-pos < sizeof(buf) ? len-pos : sizeof(buf));
            if (count <= 0 || memcmp(buf, body+pos, count)) error = EIO;
            else pos += count;
        }
        if (!error && lx_read(fd, buf, 1) != 0) error = EIO;
        if (fd >= 0) lx_close(fd);
    }
    if (!error) {
        strcpy(mail, from);
        strcpy(mail+513, to);
        if (console_call(45, 5) < 0 || console_call(45, 6) < 0) error = EIO;
    }
    if (error) console_call(45, 7);
    return error;
}

// Workspace updates share one guest filesystem revision, including conflict checks.
int lx_codex_batch(int op, const char *path, const char *other) {
    if (strlen(path) > 512 || strlen(other) > 512) return ENAMETOOLONG;
    char *mail = (char *)0x443d7000UL;
    strcpy(mail, path);
    strcpy(mail + 513, other);
    return console_call(45, op) < 0 ? EIO : 0;
}

int lx_codex_terminal(void) {
    if (console_call(24, 2) != 1 || !lx_stdout_is_terminal()) return 0;
    console_call(57, 0);  // Deliver Ctrl-C to the client for turn/interrupt.
    return 1;
}
