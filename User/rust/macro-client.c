/* Wire adapter for the pinned upstream proc_macro::bridge repr(C) ABI. The
 * compiler owns token and span handles. Only encoded byte buffers cross pipes. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct buffer {
    unsigned char *data;
    size_t len, cap;
    struct buffer (*reserve)(struct buffer, size_t);
    void (*drop)(struct buffer);
};
struct config {
    struct buffer input;
    struct buffer (*dispatch)(void *, struct buffer);
    void *context;
    bool show_panics;
};
typedef struct buffer (*macro_fn)(struct config);
extern const struct { const macro_fn *entries; size_t count; } lx_macro_table;

static void transfer(int fd, void *buf, size_t len, bool writing) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
        if (n <= 0) _Exit(2);
        p += n;
        len -= n;
    }
}
static void release(struct buffer b) { free(b.data); }
static struct buffer reserve(struct buffer b, size_t extra) {
    if (extra > SIZE_MAX - b.len) _Exit(2);
    size_t required = b.len + extra;
    b.cap = b.cap <= SIZE_MAX / 2 && 2 * b.cap > required ? 2 * b.cap : required;
    b.data = realloc(b.data, b.cap ? b.cap : 1);
    if (!b.data) _Exit(2);
    return b;
}
static struct buffer receive(unsigned char *tag) {
    uint32_t len;
    transfer(0, tag, 1, false);
    transfer(0, &len, sizeof len, false);
    if (len > (64u << 20)) _Exit(2);
    struct buffer b = reserve((struct buffer){ .reserve = reserve, .drop = release }, len);
    b.len = len;
    transfer(0, b.data, b.len, false);
    return b;
}
static void send_buffer(unsigned char tag, struct buffer b) {
    if (b.len > (64u << 20)) _Exit(2);
    uint32_t len = b.len;
    transfer(1, &tag, 1, true);
    transfer(1, &len, sizeof len, true);
    transfer(1, b.data, len, true);
    b.drop(b);
}
static struct buffer dispatch(void *unused, struct buffer b) {
    (void)unused;
    send_buffer(1, b);
    unsigned char tag;
    b = receive(&tag);
    if (tag != 0) _Exit(2);
    return b;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--macro-count")) {
        printf("%zu\n", lx_macro_table.count);
        return 0;
    }
    char *end;
    unsigned long index = strtoul(argv[1], &end, 10);
    if (!argv[1][0] || *end || index >= lx_macro_table.count) return 2;
    unsigned char flag;
    struct buffer input = receive(&flag);
    struct config config = { input, dispatch, NULL, flag != 0 };
    send_buffer(2, lx_macro_table.entries[index](config));
    return 0;
}
