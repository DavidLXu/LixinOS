#include "native.h"
#include "identity.h"

/* One bounded ring per connection. Each live endpoint owns a reference. */
#define PIPE_BYTES 8192
#define PIPE_MARKERS 16
#define PIPE_COUNT 64
struct rights_marker {
    unsigned at;
    struct shared_file *files[LX_IPC_RIGHTS];
};
struct pipe {
    unsigned readers, writers, read, write;
    unsigned first, markers;
    struct rights_marker control[PIPE_MARKERS];
    u8 data[PIPE_BYTES];
};
static struct pipe pipes[PIPE_COUNT];
unsigned pipe_ready(struct pipe *p, int write) {
    unsigned used = p->write - p->read;
    if (write)
        return used < PIPE_BYTES || !p->readers ? 2 : 0;
    return used || !p->writers;
}
struct pipe *pipe_create(void) {
    for (unsigned i = 0; i < PIPE_COUNT; i++) {
        struct pipe *p = &pipes[i];
        if (p->readers || p->writers)
            continue;
        p->readers = p->writers = 1;
        p->read = p->write = 0;
        p->first = p->markers = 0;
        return p;
    }
    return 0;
}
static void pipe_drop_rights(struct pipe *p, struct shared_file **out) {
    struct rights_marker *m = &p->control[p->first];
    for (unsigned i = 0; i < LX_IPC_RIGHTS; i++) {
        if (out) out[i] = m->files[i];
        else if (m->files[i]) shared_release(m->files[i]);
    }
    p->first = (p->first + 1) % PIPE_MARKERS;
    p->markers--;
}

void pipe_ref(struct pipe *p, int writer, int delta) {
    if (p) {
        if (writer)
            p->writers += delta;
        else {
            p->readers += delta;
            if (!p->readers)
                while (p->markers) pipe_drop_rights(p, 0);
        }
        if (!p->writers || !p->readers) task_wake(p);
    }
}
long pipe_read(struct pipe *p, u8 *dst, unsigned size) {
    if (!size)
        return 0;
    while (p->read == p->write) {
        if (!p->writers || task_stopped())
            return 0;
        task_park(p);
    }
    unsigned n = p->write - p->read;
    if (n > size)
        n = size;
    for (unsigned i = 0; i < n; i++)
        dst[i] = p->data[p->read++ % PIPE_BYTES];
    task_wake(p);
    return n;
}
long pipe_write(struct pipe *p, const u8 *src, unsigned size) {
    unsigned done = 0;
    while (done < size) {
        if (!p->readers || task_stopped())
            return done ? (long)done : -1;
        unsigned room = PIPE_BYTES - (p->write - p->read);
        if (!room) {
            task_park(p);
            continue;
        }
        if (room > size - done)
            room = size - done;
        for (unsigned i = 0; i < room; i++)
            p->data[p->write++ % PIPE_BYTES] = src[done++];
        task_wake(p);
    }
    return done;
}

enum { LOCAL_BOUND = 1, LOCAL_STREAM };
#define LOCAL_BACKLOG 4
#define LOCAL_NAMES 16
struct local_socket {
    unsigned kind,uid,gid;
    u64 pid;
    struct pipe *input, *output;
    unsigned size, backlog, count;
    u8 name[108];
    struct local_socket *pending[LOCAL_BACKLOG];
};
static struct local_socket local_sockets[LOCAL_NAMES + 2 * NATIVE_TASKS];

static struct local_socket *local_alloc(unsigned kind) {
    for (unsigned i = 0; i < sizeof(local_sockets) / sizeof(*local_sockets); i++) {
        struct local_socket *socket = &local_sockets[i];
        if (socket->kind) continue;
        memset(socket, 0, sizeof(*socket));
        socket->kind = kind;
        return socket;
    }
    return 0;
}

void local_close(struct local_socket *socket) {
    for (unsigned i = 0; i < socket->count; i++)
        local_close(socket->pending[i]);
    pipe_ref(socket->input, 0, -1);
    pipe_ref(socket->output, 1, -1);
    socket->kind = 0;
}

int local_pair(u64 left_pid, u64 right_pid, struct local_socket **left, struct local_socket **right) {
    struct local_socket *a = local_alloc(LOCAL_STREAM);
    if (!a) return -105;
    struct local_socket *b = local_alloc(LOCAL_STREAM);
    struct pipe *ab = b ? pipe_create() : 0;
    struct pipe *ba = ab ? pipe_create() : 0;
    if (!ba) {
        pipe_ref(ab, 0, -1);
        pipe_ref(ab, 1, -1);
        local_close(a);
        if (b) local_close(b);
        return -105;
    }
    a->input = ba; a->output = ab; a->pid = right_pid;
    b->input = ab; b->output = ba; b->pid = left_pid;
    a->uid=b->uid=id_uid(task_active()); a->gid=b->gid=id_gid(task_active());
    *left = a; *right = b;
    return 0;
}

static struct local_socket *local_find(const u8 *name, unsigned size) {
    for (unsigned i = 0; i < sizeof(local_sockets) / sizeof(*local_sockets); i++) {
        struct local_socket *socket = &local_sockets[i];
        if (socket->kind == LOCAL_BOUND && socket->size == size && !memcmp(socket->name, name, size))
            return socket;
    }
    return 0;
}

int local_bind(const u8 *name, unsigned size, u64 pid, struct local_socket **out) {
    if (local_find(name, size)) return -98;
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(local_sockets) / sizeof(*local_sockets); i++)
        count += local_sockets[i].kind == LOCAL_BOUND;
    if (count >= LOCAL_NAMES) return -105;
    struct local_socket *socket = local_alloc(LOCAL_BOUND);
    if (!socket) return -105;
    socket->pid = pid;
    socket->uid=id_uid(task_active());socket->gid=id_gid(task_active());
    socket->size = size;
    memcpy(socket->name, name, size);
    *out = socket;
    return 0;
}

int local_listen(struct local_socket *socket, unsigned backlog) {
    if (socket->kind != LOCAL_BOUND) return -22;
    socket->backlog = !backlog ? 1 : backlog > LOCAL_BACKLOG ? LOCAL_BACKLOG : backlog;
    return 0;
}

int local_connect(const u8 *name, unsigned size, u64 pid, struct local_socket **out) {
    struct local_socket *listener = local_find(name, size), *server;
    if (!listener || !listener->backlog) return -111;
    if (listener->count >= listener->backlog) return -11;
    int result = local_pair(pid, listener->pid, out, &server);
    if (!result) { (*out)->uid=listener->uid; (*out)->gid=listener->gid; listener->pending[listener->count++]=server; }
    return result;
}

int local_accept(struct local_socket *socket, struct local_socket **out) {
    if (socket->kind != LOCAL_BOUND || !socket->backlog) return -22;
    if (!socket->count) return -11;
    *out = socket->pending[0];
    for (unsigned i = 1; i < socket->count; i++)
        socket->pending[i-1] = socket->pending[i];
    socket->count--;
    return 0;
}

unsigned local_ready(struct local_socket *socket) {
    if (socket->kind == LOCAL_BOUND) return socket->count != 0;
    return (socket->input ? pipe_ready(socket->input, 0) : 1)
         | (socket->output && socket->output->markers == PIPE_MARKERS ? 0 :
                socket->output ? pipe_ready(socket->output, 1) : 2);
}

long local_read_rights(struct local_socket *socket, u8 *data, unsigned size, struct shared_file **files) {
    if (socket->kind != LOCAL_STREAM) return -107;
    struct pipe *p = socket->input;
    if (!size || !p) return 0;
    if (p->read == p->write) return p->writers ? -11 : 0;
    if (p->markers > 1) {
        unsigned before_next = p->control[(p->first + 1) % PIPE_MARKERS].at - p->read;
        if (size > before_next) size = before_next;
    }
    unsigned start = p->read;
    long count = pipe_read(p, data, size);
    if (count > 0 && p->markers && p->control[p->first].at - start < (unsigned)count)
        pipe_drop_rights(p, files);
    return count;
}

long local_write_rights(struct local_socket *socket, u8 *data, unsigned size, struct shared_file **files) {
    if (socket->kind != LOCAL_STREAM) return -107;
    int attach = files && files[0];
    if (!size) return attach ? -22 : 0;
    struct pipe *p = socket->output;
    if (!p || !p->readers) return -32;
    unsigned room = PIPE_BYTES - (p->write - p->read);
    if (!room || (attach && p->markers == PIPE_MARKERS)) return -11;
    unsigned start = p->write;
    long count = pipe_write(p, data, size < room ? size : room);
    if (count > 0 && attach) {
        struct rights_marker *m = &p->control[(p->first + p->markers++) % PIPE_MARKERS];
        m->at = start;
        for (unsigned i = 0; i < LX_IPC_RIGHTS; i++) {
            m->files[i] = files[i];
            if (files[i]) shared_ref(files[i]);
        }
    }
    return count;
}

long local_io(struct local_socket *socket, unsigned command, u8 *data, unsigned argument) {
    if (socket->kind != LOCAL_STREAM) return -107;
    if (command == 8) {
        if (argument > 2) return -22;
        if (argument != 1) {
            pipe_ref(socket->input, 0, -1);
            socket->input = 0;
        }
        if (argument != 0) {
            pipe_ref(socket->output, 1, -1);
            socket->output = 0;
        }
        return 0;
    }
    if (command == 9) { if (argument==8) { ((u32 *)data)[0]=socket->uid; ((u32 *)data)[1]=socket->gid; } return socket->pid; }
    if (argument > 4096) return -22;
    return command == 6 ? local_read_rights(socket, data, argument, 0)
                        : local_write_rights(socket, data, argument, 0);
}
