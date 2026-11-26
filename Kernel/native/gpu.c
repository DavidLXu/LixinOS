/* ARM64 virtio-gpu scanout and per-process VirGL contexts. All control queue
 * requests run synchronously on the primary CPU with coherent guest RAM. */
#include "virtio.h"
#include "gpu-abi.h"

enum { GET_DISPLAY = 0x100, CREATE_2D, UNREF, SET_SCANOUT, FLUSH, TRANSFER, ATTACH };
enum { OK = 0x1100, DISPLAY_INFO, BGRX = 2 };
struct gpu_header { u32 type, flags; u64 fence; u32 context, padding; };
struct rect { u32 x, y, w, h; };
struct command {
    struct gpu_header header;
    union {
        struct { u32 id, format, w, h; } create;
        struct { u32 id, entries; u64 address; u32 length, padding; } backing;
        struct { struct rect r; u32 scanout, id; } scanout;
        struct { struct rect r; u64 offset; u32 id, padding; } transfer;
        struct { struct rect r; u32 id, padding; } flush;
        struct { u32 id, padding; } unref;
        struct { u32 len, init; char name[64]; } ctx;
        struct { u32 id; struct lx_gpu_create spec; } create3;
        struct { u32 box[6]; u64 offset; u32 id, level, stride, layer; } transfer3;
        struct { u32 id, version; } cap;
    } body;
};
struct display_info {
    struct gpu_header header;
    struct { struct rect r; u32 enabled, flags; } outputs[16];
};
static struct virtio gpu = {(volatile u32 *)0x0a000e00}; /* MMIO slot 7 */
static struct queue control, cursor;
static struct command request;
static union {
    struct display_info display;
    struct { struct gpu_header header; unsigned char data[4096]; } caps;
} reply;
static u32 cap_bytes;
static unsigned char caps[4096];
static u64 fence, last_flush, interval;
static unsigned active, resource, pitch, width, height;
static u32 *framebuffer, *scanout;
static struct rect dirty;

extern u64 smp_ticks(void), smp_frequency(void);
extern void *gpu_allocate(u32 bytes);
extern void gpu_free(void *ptr, u32 bytes);

/* Persist both DMA buffers even on a device timeout. No subsequent command
 * can reuse them once the queue is offline. */
static int exchange(struct gpu_header *cmd, u32 size, u32 type, u32 ctx,
                    u32 expected, u32 response_size) {
    if (!active) return 0;
    cmd->type = type;
    /* A 2D scanout upload is followed on this queue by a fenced FLUSH.
     * Wait for its command response here and for GPU completion at FLUSH. */
    int fenced = type != TRANSFER;
    cmd->flags = fenced;
    cmd->fence = fenced ? ++fence : 0;
    cmd->context = ctx;
    cmd->padding = 0;
    reply.display.header.type = 0;
    control.desc[0].addr = (u64)cmd;
    control.desc[0].size = size;
    control.desc[0].flags = 1;
    control.desc[0].next = 1;
    control.desc[1].addr = (u64)&reply;
    control.desc[1].size = response_size;
    control.desc[1].flags = 2;
    control.avail.ring[control.avail.index % Q] = 0;
    BARRIER();
    control.avail.index++;
    BARRIER();
    gpu.r[0x50 / 4] = 0;
    u64 start = smp_ticks(), timeout = 2 * smp_frequency();
    while (control.used.index == control.seen) {
        if (smp_ticks() - start >= timeout) { active = 0; return 0; }
    }
    BARRIER();
    unsigned slot = control.seen++ % Q;
    return control.used.ring[slot].id == 0 && control.used.ring[slot].len >= response_size &&
           reply.display.header.type == expected && (!fenced || reply.display.header.fence == fence);
}
static int command(u32 type, u32 ctx, u32 body_size) {
    return exchange(&request.header, sizeof(request.header) + body_size, type, ctx, OK,
                    sizeof(request.header));
}
static int send_command(u32 type, u32 body_size) {
    if (type == GET_DISPLAY)
        return exchange(&request.header, sizeof(request.header), type, 0, DISPLAY_INFO,
                        sizeof(reply.display));
    return command(type, 0, body_size);
}
static void probe_caps(void) {
    u32 count = gpu.r[0x108 / 4];
    for (u32 i = 0; i < count && i < 64; ++i) {
        request.body.cap.id = i;
        request.body.cap.version = 0;
        if (!exchange(&request.header, sizeof(request.header) + 8, 0x108, 0, 0x1102,
                      sizeof(request.header) + 16)) continue;
        u32 info[4];
        memcpy(info, reply.caps.data, sizeof(info));
        if ((info[0] != 1 && info[0] != 2) || !info[2] || info[2] > sizeof(caps)) continue;
        request.body.cap.id = info[0];
        request.body.cap.version = info[1];
        if (exchange(&request.header, sizeof(request.header) + 8, 0x109, 0, 0x1103,
                     sizeof(request.header) + info[2])) {
            cap_bytes = info[2];
            memcpy(caps, reply.caps.data, cap_bytes);
            if (info[0] == 2) break;
        }
    }
}
int gpu_init(void) {
    gpu.r[0x14 / 4] = 0;
    unsigned virgl = gpu.r[0x10 / 4] & 1;
    if (!device(gpu, 16, virgl) || !setup_queue(gpu, 0, &control) || !setup_queue(gpu, 1, &cursor))
        return 0;
    gpu.r[0x70 / 4] = 15;
    active = 1;
    interval = smp_frequency() / 60;
    if (!send_command(GET_DISPLAY, 0) || !reply.display.outputs[0].enabled ||
        !reply.display.outputs[0].r.w || !reply.display.outputs[0].r.h) active = 0;
    if (active && virgl) probe_caps();
    return active;
}
static int unref(u32 id) {
    request.body.unref.id = id;
    request.body.unref.padding = 0;
    return send_command(UNREF, sizeof(request.body.unref));
}
int gpu_set_mode(u32 *fb, u32 stride, u32 w, u32 h) {
    if (!active || !w || !h || w > stride || (u64)stride * h > 0x3fffffff) return 0;
    u32 id = resource == 1 ? 2 : 1;
    request.body.create.id = id;
    request.body.create.format = BGRX;
    request.body.create.w = w;
    request.body.create.h = h;
    if (!send_command(CREATE_2D, sizeof(request.body.create))) return 0;
    /* GL frontends sample the full resource. Keep its rows tightly packed
     * even when the guest drawing surface has padding between rows. */
    u32 bytes = w * h * 4;
    u32 *pixels = stride == w ? fb : gpu_allocate(bytes);
    if (!pixels) { unref(id); return 0; }
    request.body.backing.id = id;
    request.body.backing.entries = 1;
    request.body.backing.address = (u64)pixels;
    request.body.backing.length = bytes;
    request.body.backing.padding = 0;
    if (!send_command(ATTACH, sizeof(request.body.backing))) goto discard;
    request.body.scanout.r = (struct rect){0, 0, w, h};
    request.body.scanout.scanout = 0;
    request.body.scanout.id = id;
    if (!send_command(SET_SCANOUT, sizeof(request.body.scanout))) goto discard;
    if (resource && unref(resource) && scanout != framebuffer)
        gpu_free(scanout, width * height * 4);
    resource = id;
    framebuffer = fb;
    scanout = pixels;
    pitch = stride;
    width = w;
    height = h;
    dirty = (struct rect){0, 0, w, h};
    return 1;
discard:
    if (unref(id) && pixels != fb) gpu_free(pixels, bytes);
    return 0;
}
void gpu_damage(u32 x, u32 y, u32 w, u32 h) {
    if (!active || x >= width || y >= height || !w || !h) return;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    if (dirty.w) {
        u32 right = dirty.x + dirty.w, bottom = dirty.y + dirty.h;
        if (right < x + w) right = x + w;
        if (bottom < y + h) bottom = y + h;
        if (x < dirty.x) dirty.x = x;
        if (y < dirty.y) dirty.y = y;
        dirty.w = right - dirty.x;
        dirty.h = bottom - dirty.y;
    } else {
        dirty = (struct rect){x,y,w,h};
    }
}
void gpu_refresh(void) {
    if (!active || !dirty.w) return;
    u64 now = smp_ticks();
    if (now - last_flush < interval) return;
    /* Pace frame starts. The synchronous upload and flush consume part of
     * this frame's budget, especially on a VirGL host. */
    last_flush = now;
    if (scanout != framebuffer) {
        for (u32 y = dirty.y; y < dirty.y + dirty.h; ++y)
            memcpy(scanout + y * width + dirty.x, framebuffer + y * pitch + dirty.x, dirty.w * 4);
    }
    request.body.transfer.r = dirty;
    request.body.transfer.offset = ((u64)dirty.y * width + dirty.x) * 4;
    request.body.transfer.id = resource;
    request.body.transfer.padding = 0;
    if (!send_command(TRANSFER, sizeof(request.body.transfer))) return;
    request.body.flush.r = dirty;
    request.body.flush.id = resource;
    request.body.flush.padding = 0;
    if (send_command(FLUSH, sizeof(request.body.flush))) dirty.w = 0;
}

enum { CTX_CREATE = 0x200, CTX_DESTROY, CTX_ATTACH, CTX_DETACH, CREATE_3D,
       TO_HOST, FROM_HOST, SUBMIT_3D, MAX_RESOURCES = 512, MAX_CONTEXTS = 32 };
struct submit_buffer { struct gpu_header header; u32 bytes, pad; u8 data[LX_GPU_COMMAND_MAX]; };
struct gpu_session { struct submit_buffer *submit; u32 staged, allocated; };
struct gpu_resource { u32 owner, bytes; u8 *backing; };
static struct gpu_session sessions[MAX_CONTEXTS];
static struct gpu_resource resources[MAX_RESOURCES];

static int resource_command(u32 type, u32 ctx, u32 id) {
    request.body.unref.id = id;
    request.body.unref.padding = 0;
    return command(type, ctx, sizeof(request.body.unref));
}
static int drop_resource(u32 ctx, u32 id) {
    struct gpu_resource *r = &resources[id - 3];
    if (!resource_command(UNREF, 0, id)) return 0; /* Retain backing while device owns it. */
    if (r->backing) gpu_free(r->backing, r->bytes);
    sessions[ctx-1].allocated -= r->bytes;
    memset(r, 0, sizeof(*r));
    return 1;
}
void gpu_close(u32 *owner) {
    u32 ctx = *owner;
    if (!ctx) return;
    *owner = 0;
    if (!command(CTX_DESTROY, ctx, 0)) return;
    for (u32 i = 0; i < MAX_RESOURCES; ++i)
        if (resources[i].owner == ctx && !drop_resource(ctx, i+3)) return;
    struct gpu_session *s = &sessions[ctx-1];
    gpu_free(s->submit, sizeof(*s->submit));
    memset(s, 0, sizeof(*s));
}
static long open_context(u32 *owner) {
    if (*owner || !active || !cap_bytes) return -1;
    for (u32 i = 0; i < MAX_CONTEXTS; ++i) {
        struct gpu_session *s = &sessions[i];
        if (s->submit) continue;
        s->submit = gpu_allocate(sizeof(*s->submit));
        if (!s->submit) return -1;
        memset(&request.body.ctx, 0, sizeof(request.body.ctx));
        memcpy(request.body.ctx.name, "LixinOS", 7);
        request.body.ctx.len = 7;
        if (!command(CTX_CREATE, i+1, sizeof(request.body.ctx))) {
            gpu_free(s->submit, sizeof(*s->submit));
            s->submit = 0;
            return -1;
        }
        *owner = i+1;
        return 0;
    }
    return -1;
}
static long create_resource(u32 ctx, const struct lx_gpu_create *spec) {
    struct gpu_session *s = &sessions[ctx-1];
    if (spec->bytes > (64u<<20) - s->allocated || spec->last_level > 31) return -1;
    u32 i = 0;
    while (i < MAX_RESOURCES && resources[i].owner) ++i;
    if (i == MAX_RESOURCES) return -1;
    u32 id = i+3;
    u8 *backing = spec->bytes ? gpu_allocate(spec->bytes) : 0;
    if (spec->bytes && !backing) return -1;
    request.body.create3.id = id;
    request.body.create3.spec = *spec;
    request.body.create3.spec.bytes = 0; /* wire padding */
    if (!command(CREATE_3D, ctx, sizeof(request.body.create3))) {
        if (backing) gpu_free(backing, spec->bytes);
        return -1;
    }
    resources[i] = (struct gpu_resource){ctx, spec->bytes, backing};
    s->allocated += spec->bytes;
    if (backing) {
        request.body.backing.id = id;
        request.body.backing.entries = 1;
        request.body.backing.address = (u64)backing;
        request.body.backing.length = spec->bytes;
        request.body.backing.padding = 0;
        if (!send_command(ATTACH, sizeof(request.body.backing))) goto fail;
    }
    if (resource_command(CTX_ATTACH, ctx, id)) return id;
fail:
    drop_resource(ctx, id);
    return -1;
}
long gpu_call(u32 *owner, u64 operation, u8 *mail) {
    if (operation == LX_GPU_OPEN) return open_context(owner);
    u32 ctx = *owner;
    if (!ctx || !active) return -1;
    struct gpu_session *s = &sessions[ctx-1];
    struct lx_gpu_chunk *chunk = (struct lx_gpu_chunk *)mail;
    if (operation == LX_GPU_CLOSE) { gpu_close(owner); return 0; }
    if (operation == LX_GPU_CAPS) { memcpy(mail, caps, cap_bytes); return cap_bytes; }
    if (operation == LX_GPU_CREATE) return create_resource(ctx, (struct lx_gpu_create *)mail);
    if (operation == LX_GPU_COMMAND_PART) {
        if (!chunk->offset) s->staged = 0;
        if (chunk->offset != s->staged || chunk->bytes > LX_GPU_MAIL_SIZE-sizeof(*chunk) ||
            chunk->bytes > LX_GPU_COMMAND_MAX-s->staged) return -1;
        memcpy(s->submit->data+s->staged, chunk->data, chunk->bytes);
        s->staged += chunk->bytes;
        return 0;
    }
    if (operation == LX_GPU_SUBMIT) {
        u32 n = s->staged;
        s->staged = 0;
        if (!n || n % 4 || n != *(u32 *)mail) return -1;
        s->submit->bytes = n;
        s->submit->pad = 0;
        return exchange(&s->submit->header, sizeof(struct gpu_header)+8+n, SUBMIT_3D, ctx,
                        OK, sizeof(struct gpu_header)) ? 0 : -1;
    }
    u32 id = chunk->id;
    if (id < 3 || id-3 >= MAX_RESOURCES || resources[id-3].owner != ctx) return -1;
    struct gpu_resource *r = &resources[id-3];
    if (operation == LX_GPU_DROP) {
        if (!resource_command(CTX_DETACH, ctx, id)) return -1;
        return drop_resource(ctx, id) ? 0 : -1;
    }
    if (operation == LX_GPU_READ || operation == LX_GPU_WRITE) {
        if (!r->backing || chunk->offset > r->bytes || chunk->bytes > r->bytes-chunk->offset ||
            chunk->bytes > LX_GPU_MAIL_SIZE-sizeof(*chunk)) return -1;
        if (operation == LX_GPU_READ) memcpy(chunk->data, r->backing+chunk->offset, chunk->bytes);
        else memcpy(r->backing+chunk->offset, chunk->data, chunk->bytes);
        return 0;
    }
    if (operation == LX_GPU_UPLOAD || operation == LX_GPU_DOWNLOAD) {
        struct lx_gpu_transfer *t = (struct lx_gpu_transfer *)mail;
        if (!r->backing || t->offset >= r->bytes || !t->width || !t->height || !t->depth) return -1;
        memcpy(request.body.transfer3.box, &t->x, 6*sizeof(u32));
        request.body.transfer3.offset = t->offset;
        request.body.transfer3.id = id;
        request.body.transfer3.level = t->level;
        request.body.transfer3.stride = t->stride;
        request.body.transfer3.layer = t->layer_stride;
        return command(operation == LX_GPU_UPLOAD ? TO_HOST : FROM_HOST, ctx,
                       sizeof(request.body.transfer3)) ? 0 : -1;
    }
    return -1;
}
