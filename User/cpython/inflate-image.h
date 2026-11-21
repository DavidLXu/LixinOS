// The bootstrap ELF is read-only. Use scratch above the loaded program's stack
// for zlib, before musl starts using that region for its own heap.
#include <zlib.h>

struct inflate_scratch {
    unsigned char input[4096], window[32768];
    uintptr_t state[2048];
};
struct inflate_piece_state {
    struct inflate_scratch *scratch;
    const struct piece *piece;
    int fd;
    unsigned offset;
    uint32_t checksum;
};

void *memset(void *dest, int byte, size_t size) {
    unsigned char *out = dest;
    while (size--) *out++ = byte;
    return dest;
}

void *memcpy(void *dest, const void *source, size_t size) {
    unsigned char *out = dest;
    const unsigned char *in = source;
    while (size--) *out++ = *in++;
    return dest;
}

voidpf zcalloc(voidpf opaque, unsigned items, unsigned size) {
    struct inflate_piece_state *s = opaque;
    if (!items || size > sizeof(s->scratch->state) / items) return Z_NULL;
    // inflateBackInit allocates one inflate_state and uses our external window.
    return s->scratch->state;
}
void zcfree(voidpf opaque, voidpf ptr) { (void)opaque; (void)ptr; }

static unsigned compressed_input(void *opaque, z_const unsigned char **buf) {
    struct inflate_piece_state *s = opaque;
    *buf = s->scratch->input;
    int n = lx_read(s->fd, s->scratch->input, sizeof(s->scratch->input));
    return n > 0 ? (unsigned)n : 0;
}
static int image_output(void *opaque, unsigned char *buf, unsigned size) {
    struct inflate_piece_state *s = opaque;
    if (size > s->piece->size - s->offset) return 1;
    unsigned char *out = (unsigned char *)s->piece->dest + s->offset;
    for (unsigned i = 0; i < size; ++i) {
        out[i] = buf[i];
        s->checksum = (s->checksum ^ buf[i]) * 16777619U;
    }
    s->offset += size;
    return 0;
}
static int inflate_piece(int fd, const struct piece *p) {
    struct inflate_scratch *scratch = (void *)STACK_TOP;
    if (console_call(37, STACK_TOP - IMAGE_BASE + sizeof(*scratch)) != IMAGE_BASE) return 0;
    struct inflate_piece_state state = {scratch, p, fd, 0, 2166136261U};
    z_stream stream = {.zalloc = zcalloc, .zfree = zcfree, .opaque = &state};
    if (inflateBackInit(&stream, 15, scratch->window) != Z_OK) return 0;
    int result = inflateBack(&stream, compressed_input, &state, image_output, &state);
    int complete = result == Z_STREAM_END && !stream.avail_in &&
                   lx_read(fd, scratch->input, 1) == 0;
    inflateBackEnd(&stream);
    return complete && state.offset == p->size && state.checksum == p->checksum;
}
