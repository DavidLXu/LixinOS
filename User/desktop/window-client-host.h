#pragma once
#include "window-client.h"
#ifdef __TINYC__
void *realloc(void *, unsigned long);
void free(void *);
#else
#include <stdlib.h>
#endif

enum { CLIENT_EVENTS = 64 };

typedef struct {
    int pid, input, output, stopping;
    unsigned received, clipboard_bytes, header_bytes, scratch_capacity, encoded_capacity;
    int adaptive, extended, right_click, configuring, target_w, target_h, width, height;
    unsigned interval;
    long next_tick, close_deadline;
    const char *error;
    WindowReply reply;
    char clipboard[WINDOW_CLIENT_CLIPBOARD_MAX];
    char paste[WINDOW_CLIENT_CLIPBOARD_MAX];
    unsigned short *pixels, *scratch, *encoded;
    int busy, ready, head, count, sent, overflow, paste_queued;
    long close_requested_at;
    int awaiting_close_reply, confirm_force_close;
    int pointer_x, pointer_y;
    WindowEvent events[CLIENT_EVENTS];
} WindowClient;

static int client_open_streams(WindowClient *client, const char *cwd, const char *path, const char *argument, int error_fd) {
    if (client->pid) return 0;
    long input = console_call(49, 0);
    if (input < 0) return 0;
    long output = console_call(49, 0);
    int pid = -1;
    if (output >= 0) {
        char *mail = (char *)0x443d8000;
        unsigned a = strlen(cwd) + 1, b = strlen(path) + 1;
        memcpy(mail, cwd, a);
        memcpy(mail + a, path, b);
        memcpy(mail + a + b, path, b);
        unsigned used = a + 2*b;
        if (argument) {
            unsigned n = strlen(argument) + 1;
            memcpy(mail + used, argument, n);
            used += n;
        }
        pid = console_call(50, ((long)used << 32) | ((long)error_fd << 16) |
                           ((output & 255) << 8) | (input >> 32));
        lx_close(output & 255);
        if (pid <= 0) lx_close(output >> 32);
    }
    lx_close(input >> 32);
    if (pid <= 0) {
        lx_close(input & 255);
        return 0;
    }
    memset(client, 0, sizeof *client);
    size_t length = strlen(path);
    client->extended = length > 5 && !strcmp(path + length - 5, ".win3");
    client->adaptive = client->extended || (length > 5 && !strcmp(path + length - 5, ".win2"));
    client->target_w = WINDOW_CLIENT_WIDTH;
    client->target_h = WINDOW_CLIENT_HEIGHT;
    client->pid = pid;
    client->input = input & 255;
    client->output = output >> 32;
    return 1;
}

static inline int client_open_argument(WindowClient *client, const char *cwd, const char *path, const char *argument) {
    return client_open_streams(client, cwd, path, argument, 255);
}

static inline int client_open(WindowClient *client, const char *cwd, const char *path) {
    return client_open_argument(client,cwd,path,0);
}

static void client_event(WindowClient *client, WindowEvent event) {
    if (!client->pid || client->stopping) return;
    int tail = (client->head + client->count) % CLIENT_EVENTS;
    WindowEvent *previous = &client->events[(tail + CLIENT_EVENTS - 1) % CLIENT_EVENTS];
    int can_replace = client->count > (client->sent != 0);
    if (event.type == WINDOW_CLIENT_CLOSE && client->count && previous->type == event.type)
        return;
    if (event.type == WINDOW_CLIENT_CONFIGURE && can_replace && previous->type == event.type) {
        *previous = event;
        return;
    }
    if (event.type == WINDOW_CLIENT_POINTER) {
        client->pointer_x = event.x;
        client->pointer_y = event.y;
        if (can_replace && previous->type == WINDOW_CLIENT_POINTER && previous->value == event.value) {
            *previous = event;
            return;
        }
    }
    if (client->count == CLIENT_EVENTS) {
        client->overflow = 1;
        return;
    }
    client->events[tail] = event;
    client->count++;
}

static void client_close(WindowClient *client) {
    if (!client->pid || client->stopping) return;
    client->close_deadline = console_call(38, 0) + 250;
    lx_close(client->input);
    lx_close(client->output);
    free(client->pixels);
    free(client->scratch);
    free(client->encoded);
    client->pixels = client->scratch = client->encoded = NULL;
    client->stopping = 1;
}

static void client_collect_paste(WindowClient *c) {
    if (c->paste_queued || c->count == CLIENT_EVENTS || c->stopping) return;
    long n = console_call(35, 0);
    if (n <= 0 || n > sizeof c->paste) return;
    memcpy(c->paste, (const char *)0x443d8000UL, n);
    client_event(c, (WindowEvent){.type = WINDOW_CLIENT_PASTE, .value = n});
    c->paste_queued = 1;
}

static void client_shutdown(WindowClient *client) {
    if (!client->pid) return;
    client_close(client);
    console_call(55, client->pid);
}

/* Validate the rectangle against the configure actually delivered on stdin.
 * A later resize may already be queued while this reply is still arriving. */
static int prepare_patch(WindowClient *c) {
    WindowUpdate u = c->reply.frame;
    int full = !u.x && !u.y && u.w == c->target_w && u.h == c->target_h;
    if (u.x + u.w > c->target_w || u.y + u.h > c->target_h ||
        ((!u.w || !u.h) && (u.x || u.y || u.w || u.h)) ||
        ((!c->ready || c->configuring) && !full) ||
        c->reply.clipboard_size > sizeof c->clipboard) {
        c->error = "Application sent an invalid redraw rectangle";
        return 0;
    }
    unsigned bytes = (unsigned)u.w * u.h * sizeof *c->scratch;
    unsigned encoded = c->reply.encoded_size;
    if (encoded && (encoded % 4 || encoded >= bytes)) {
        c->error = "Application sent an invalid encoded redraw";
        return 0;
    }
    if (bytes > c->scratch_capacity) {
        void *buffer = realloc(c->scratch, bytes);
        if (!buffer) {
            c->error = "Not enough memory for application redraw";
            return 0;
        }
        c->scratch = buffer;
        c->scratch_capacity = bytes;
    }
    if (encoded > c->encoded_capacity) {
        void *buffer = realloc(c->encoded, encoded);
        if (!buffer) {
            c->error = "Not enough memory for encoded redraw";
            return 0;
        }
        c->encoded = buffer;
        c->encoded_capacity = encoded;
    }
    return 1;
}

static int commit_patch(WindowClient *c) {
    WindowUpdate u = c->reply.frame;
    if (c->reply.encoded_size) {
        unsigned total = (unsigned)u.w * u.h, at = 0;
        for (unsigned i = 0; i < c->reply.encoded_size / 2; i += 2) {
            unsigned run = c->encoded[i];
            if (!run || run > total - at) {
                c->error = "Application sent an invalid pixel run";
                return -1;
            }
            while (run--) c->scratch[at++] = c->encoded[i + 1];
        }
        if (at != total) {
            c->error = "Application sent an incomplete encoded redraw";
            return -1;
        }
    }
    c->right_click = c->extended && (u.timer_ms & WINDOW_CLIENT_WANT_RIGHT_CLICK);
    unsigned interval = u.timer_ms & ~WINDOW_CLIENT_WANT_RIGHT_CLICK;
    if (!c->extended) interval = u.timer_ms;
    if (interval && interval < 16) interval = 16;
    if (interval != c->interval) {
        c->interval = interval;
        c->next_tick = console_call(38, 0) + interval;
    }
    c->busy = 0;
    if (c->awaiting_close_reply) {
        c->awaiting_close_reply = 0;
        c->close_requested_at = 0;
        c->confirm_force_close = 0;
    }
    if (c->reply.clipboard_size)
        lx_copy_text(c->clipboard, c->reply.clipboard_size);
    if (!u.w) return 0;
    if (u.w == c->target_w && u.h == c->target_h) {
        unsigned short *old = c->pixels;
        c->pixels = c->scratch;
        c->scratch = old;
        c->scratch_capacity = (unsigned)c->width * c->height * sizeof *old;
        c->width = u.w;
        c->height = u.h;
    } else {
        for (int y = 0; y < u.h; ++y)
            memcpy(c->pixels + (u.y + y) * c->width + u.x, c->scratch + y*u.w, u.w * sizeof *c->pixels);
    }
    c->ready = 1;
    return 1;
}

static int receive_patch(WindowClient *c) {
    unsigned header_size = c->extended ? sizeof c->reply : sizeof c->reply.frame;
    for (int i = 0; i < 16; ++i) {
        unsigned bytes = c->reply.encoded_size ? c->reply.encoded_size :
            (unsigned)c->reply.frame.w * c->reply.frame.h * sizeof *c->pixels;
        if (c->header_bytes == header_size && c->received == bytes &&
            c->clipboard_bytes == c->reply.clipboard_size)
            return commit_patch(c);
        int readable = lx_poll(c->output, LX_READABLE, 0);
        if (readable <= 0) return readable < 0 ? -1 : 0;
        if (c->header_bytes < header_size) {
            int n = lx_read(c->output, (char *)&c->reply + c->header_bytes, header_size - c->header_bytes);
            if (n <= 0) return -1;
            c->header_bytes += n;
            if (c->header_bytes == header_size) {
                if (!prepare_patch(c)) return -1;
            }
        } else if (c->received < bytes) {
            char *target = (char *)(c->reply.encoded_size ? c->encoded : c->scratch);
            int n = lx_read(c->output, target + c->received, bytes - c->received);
            if (n <= 0) return -1;
            c->received += n;
        } else {
            int n = lx_read(c->output, c->clipboard + c->clipboard_bytes,
                            c->reply.clipboard_size - c->clipboard_bytes);
            if (n <= 0) return -1;
            c->clipboard_bytes += n;
        }
    }
    return 0;
}

static int client_tick(WindowClient *client, int visible) {
    if (!client->pid) return 0;
    if (console_call(51, (1L << 32) | client->pid) != -2) {
        int was_open = !client->stopping;
        client_close(client);
        client->pid = 0;
        return was_open ? -1 : 0;
    }
    if (client->stopping) {
        if (client->stopping == 1 && console_call(38, 0) >= client->close_deadline) {
            console_call(55, client->pid);
            client->stopping = 2;
        }
        return 0;
    }
    if (client->overflow) return -1;
    if (client->busy) return receive_patch(client);
    if (!client->count && visible && client->interval) {
        long now = console_call(38, 0);
        if (now >= client->next_tick) {
            client_event(client, (WindowEvent){WINDOW_CLIENT_TIMER, 0, 0, 0});
            client->next_tick = now + client->interval;
        }
    }
    if (!client->count) return 0;
    WindowEvent *event = &client->events[client->head];
    int length = sizeof *event + (event->type == WINDOW_CLIENT_PASTE ? event->value : 0);
    /* Both guest kernels guarantee one writable byte after poll on this
     * single-writer pipe. TCC's blocking write waits for the entire count. */
    for (int budget = 128; client->sent < length && budget; --budget) {
        int ready = lx_poll(client->input, LX_WRITABLE, 0);
        if (ready <= 0) return ready;
        const char *byte = client->sent < (int)sizeof *event ? (const char *)event + client->sent :
                           client->paste + (client->sent - sizeof *event);
        if (lx_write(client->input, byte, 1) != 1) return -1;
        ++client->sent;
    }
    if (client->sent < length) return 0;
    client->sent = 0;
    if (event->type == WINDOW_CLIENT_PASTE) client->paste_queued = 0;
    client->awaiting_close_reply = event->type == WINDOW_CLIENT_CLOSE;
    client->configuring = event->type == WINDOW_CLIENT_CONFIGURE;
    if (client->configuring) {
        client->target_w = event->x;
        client->target_h = event->y;
    }
    client->header_bytes = client->received = client->clipboard_bytes = 0;
    client->reply = (WindowReply){0};
    if (!client->adaptive) {
        client->reply.frame = (WindowUpdate){0, 0, WINDOW_CLIENT_WIDTH, WINDOW_CLIENT_HEIGHT, 0};
        client->header_bytes = sizeof client->reply.frame;
        if (!prepare_patch(client)) return -1;
    }
    client->busy = 1;
    client->head = (client->head + 1) % CLIENT_EVENTS;
    --client->count;
    return 0;
}
