/* In-process transport for GTK 3's software-rendered Broadway GDK types. */
#include "config.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <glib-unix.h>
#include "gdkbroadway-server.h"
#include "gdkdisplay-broadway.h"
#include "gdkeventsource.h"
#include "window-client.h"

typedef struct {
    int id, x, y, w, h, transient;
    gboolean visible, configure, popup;
    cairo_surface_t *surface;
} Surface;

struct _GdkBroadwayServer {
    GObject parent;
    GPtrArray *windows;
    int width, height, primary, focused, hover, grab, x, y, next_id;
    guint32 serial, time;
    guint read_source, reply_source;
    gboolean right_click;
    gboolean down, dirty;
    char key_text[6];
    unsigned key_bytes;
    char escape[12];
    unsigned escape_size;
    gint64 escape_time;
};
struct _GdkBroadwayServerClass { GObjectClass parent; };
G_DEFINE_TYPE(GdkBroadwayServer, gdk_broadway_server, G_TYPE_OBJECT)

static void surface_free(gpointer data)
{
    Surface *surface = data;
    if (surface->surface) cairo_surface_destroy(surface->surface);
    g_free(surface);
}

static void dispose(GObject *object)
{
    GdkBroadwayServer *server = GDK_BROADWAY_SERVER(object);
    if (server->read_source) g_clear_handle_id(&server->read_source, g_source_remove);
    if (server->reply_source) g_clear_handle_id(&server->reply_source, g_source_remove);
    g_clear_pointer(&server->windows, g_ptr_array_unref);
    G_OBJECT_CLASS(gdk_broadway_server_parent_class)->dispose(object);
}

static void gdk_broadway_server_class_init(GdkBroadwayServerClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = dispose;
}
static void gdk_broadway_server_init(GdkBroadwayServer *server)
{
    server->windows = g_ptr_array_new_with_free_func(surface_free);
    server->next_id = 1;
    server->dirty = TRUE;
}

static Surface *find(GdkBroadwayServer *server, int id)
{
    for (guint i = 0; i < server->windows->len; ++i) {
        Surface *surface = g_ptr_array_index(server->windows, i);
        if (surface->id == id) return surface;
    }
    return NULL;
}

static gboolean transfer(int fd, void *data, size_t size)
{
    char *bytes = data;
    while (size) {
        if (fd == STDOUT_FILENO) {
            struct pollfd f = {fd, POLLOUT, 0};
            int n = poll(&f, 1, -1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || !(f.revents & POLLOUT)) return FALSE;
        }
        ssize_t n = fd == STDIN_FILENO ? read(fd, bytes, size) : write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return FALSE;
        size -= n;
        bytes += n;
    }
    return TRUE;
}

static void dispatch(GdkBroadwayServer *server, BroadwayInputMsg *msg)
{
    msg->base.serial = ++server->serial;
    msg->base.time = server->time = g_get_monotonic_time() / 1000;
    _gdk_broadway_events_got_input(msg);
}

static gboolean configure_windows(gpointer data)
{
    GdkBroadwayServer *server = data;
    for (guint i = 0; i < server->windows->len; ++i) {
        Surface *s = g_ptr_array_index(server->windows, i);
        if (!s->configure) continue;
        s->configure = FALSE;
        BroadwayInputMsg msg = {.configure_notify = {.base.type = BROADWAY_EVENT_CONFIGURE_NOTIFY,
            .id = s->id, .x = s->x, .y = s->y, .width = s->w, .height = s->h}};
        dispatch(server, &msg);
    }
    return G_SOURCE_REMOVE;
}

static double surface_scale(const GdkBroadwayServer *server, const Surface *s)
{
    return MIN(1.0, MIN((double)server->width / MAX(s->w, 1),
                       (double)server->height / MAX(s->h, 1)));
}

static Surface *hit(GdkBroadwayServer *server, int x, int y)
{
    for (guint i = server->windows->len; i; --i) {
        Surface *s = g_ptr_array_index(server->windows, i - 1);
        double scale = surface_scale(server, s);
        if (s->visible && x >= s->x && y >= s->y && x - s->x < s->w * scale && y - s->y < s->h * scale)
            return s;
    }
    return NULL;
}

static void pointer_event(GdkBroadwayServer *server, int type, int id, int argument)
{
    Surface *s = find(server, id);
    if (!s) return;
    double scale = surface_scale(server, s);
    int x = (server->x - s->x) / scale, y = (server->y - s->y) / scale;
    BroadwayInputMsg msg = {.pointer = {
        .base.type = type, .mouse_window_id = server->hover, .event_window_id = id,
        .root_x = s->x + x, .root_y = s->y + y, .win_x = x, .win_y = y,
        .state = server->down ? GDK_BUTTON1_MASK : 0
    }};
    if (type == BROADWAY_EVENT_SCROLL) msg.scroll.dir = argument;
    if (type == BROADWAY_EVENT_BUTTON_PRESS || type == BROADWAY_EVENT_BUTTON_RELEASE) {
        msg.button.button = argument;
        if (type == BROADWAY_EVENT_BUTTON_RELEASE && argument == 3)
            msg.pointer.state |= GDK_BUTTON3_MASK;
    }
    dispatch(server, &msg);
}

static void key_event(GdkBroadwayServer *server, guint key, guint mods)
{
    if (!key || !server->focused) return;
    BroadwayInputMsg msg = {.key = {.base.type = BROADWAY_EVENT_KEY_PRESS,
        .window_id = server->focused, .key = key, .state = mods}};
    dispatch(server, &msg);
    msg.base.type = BROADWAY_EVENT_KEY_RELEASE;
    dispatch(server, &msg);
}

static void keyboard(GdkBroadwayServer *server, guint k)
{
    static const struct { const char *sequence; guint key; } navigation[] = {
        {"[A", GDK_KEY_Up}, {"[B", GDK_KEY_Down}, {"[C", GDK_KEY_Right}, {"[D", GDK_KEY_Left},
        {"OA", GDK_KEY_Up}, {"OB", GDK_KEY_Down}, {"OC", GDK_KEY_Right}, {"OD", GDK_KEY_Left},
        {"[H", GDK_KEY_Home}, {"[F", GDK_KEY_End}, {"OH", GDK_KEY_Home}, {"OF", GDK_KEY_End},
        {"[1~", GDK_KEY_Home}, {"[4~", GDK_KEY_End}, {"[7~", GDK_KEY_Home}, {"[8~", GDK_KEY_End},
        {"[2~", GDK_KEY_Insert}, {"[3~", GDK_KEY_Delete},
        {"[5~", GDK_KEY_Page_Up}, {"[6~", GDK_KEY_Page_Down}
    };
    if (server->escape_size) {
        if (server->escape_size > 1 || k == '[' || k == 'O') {
            if (server->escape_size == sizeof server->escape) {
                server->escape_size = 0;
                return;
            }
            server->escape[server->escape_size++ - 1] = k;
            server->escape[server->escape_size - 1] = 0;
            if (server->escape_size > 2 && k >= 0x40 && k <= 0x7e) {
                for (gsize i = 0; i < G_N_ELEMENTS(navigation); ++i) {
                    if (!strcmp(server->escape, navigation[i].sequence)) {
                        key_event(server, navigation[i].key, 0);
                        break;
                    }
                }
                server->escape_size = 0;
            }
            return;
        }
        server->escape_size = 0;
        key_event(server, GDK_KEY_Escape, 0);
    }
    switch (k) {
    case 8: case 127: key_event(server, GDK_KEY_BackSpace, 0); return;
    case 9: key_event(server, GDK_KEY_Tab, 0); return;
    case 10: case 13: key_event(server, GDK_KEY_Return, 0); return;
    case 27:
        server->escape_size = 1;
        server->escape_time = g_get_monotonic_time();
        return;
    }
    if (k >= 1 && k <= 26) {
        key_event(server, GDK_KEY_a + k - 1, GDK_CONTROL_MASK);
    } else if (k >= 32 && k <= 126) {
        key_event(server, k, 0);
    } else if (k > 127 && k <= 255) {
        if ((k & 0xc0) != 0x80) server->key_bytes = 0;
        if (server->key_bytes == 4) server->key_bytes = 0;
        server->key_text[server->key_bytes++] = k;
        gunichar ch = g_utf8_get_char_validated(server->key_text, server->key_bytes);
        if (ch != (gunichar)-2) {
            if (ch != (gunichar)-1) key_event(server, gdk_unicode_to_keyval(ch), 0);
            server->key_bytes = 0;
        }
    }
}

static gboolean geometry(GdkBroadwayServer *server, WindowEvent event)
{
    if (event.x < 1 || event.y < 1 || event.x > 2048 || event.y > 1152) return FALSE;
    server->width = event.x;
    server->height = event.y;
    server->right_click = (event.value & WINDOW_CLIENT_RIGHT_CLICK_AVAILABLE) != 0;
    server->dirty = TRUE;
    return TRUE;
}

static gboolean reply(gpointer data)
{
    GdkBroadwayServer *server = data;
    server->reply_source = 0;
    WindowReply frame = {.frame.timer_ms = 32 | (server->right_click ? WINDOW_CLIENT_WANT_RIGHT_CLICK : 0)};
    guint16 *pixels = NULL;
    size_t count = (size_t)server->width * server->height;
    if (server->dirty) {
        cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, server->width, server->height);
        cairo_t *cr = cairo_create(canvas);
        cairo_set_source_rgb(cr, .94, .94, .94);
        cairo_paint(cr);
        for (guint i = 0; i < server->windows->len; ++i) {
            Surface *s = g_ptr_array_index(server->windows, i);
            if (!s->visible || !s->surface) continue;
            double scale = surface_scale(server, s);
            cairo_save(cr);
            cairo_translate(cr, s->x, s->y);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, s->surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        gboolean valid = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
        cairo_destroy(cr);
        if (valid) {
            cairo_surface_flush(canvas);
            const guint32 *rgb = (const guint32 *)cairo_image_surface_get_data(canvas);
            size_t stride = cairo_image_surface_get_stride(canvas) / sizeof *rgb;
            pixels = g_new(guint16, count);
            for (size_t y = 0; y < (size_t)server->height; ++y) {
                for (size_t x = 0; x < (size_t)server->width; ++x) {
                    guint32 p = rgb[y * stride + x];
                    pixels[y * server->width + x] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f);
                }
            }
            frame.frame.w = server->width;
            frame.frame.h = server->height;
        }
        cairo_surface_destroy(canvas);
        server->dirty = FALSE;
    }
    guint16 *runs = pixels ? g_new(guint16, count) : NULL;
    if (pixels) {
        size_t used = 0, start = 0;
        while (start < count && used + 2 < count) {
            size_t end = start + 1;
            while (end < count && end - start < 65535 && pixels[end] == pixels[start]) ++end;
            runs[used++] = end - start;
            runs[used++] = pixels[start];
            start = end;
        }
        if (start == count) frame.encoded_size = used * sizeof *runs;
    }
    gboolean ok = transfer(STDOUT_FILENO, &frame, sizeof frame);
    if (ok && pixels)
        ok = transfer(STDOUT_FILENO, frame.encoded_size ? runs : pixels,
                      frame.encoded_size ? frame.encoded_size : count * sizeof *pixels);
    g_free(runs);
    g_free(pixels);
    if (!ok) _exit(1);
    return G_SOURCE_REMOVE;
}

static gboolean read_event(gint fd, GIOCondition condition, gpointer data)
{
    GdkBroadwayServer *server = data;
    WindowEvent event;
    if (!(condition & G_IO_IN) || !transfer(fd, &event, sizeof event)) _exit(0);
    switch (event.type) {
    case WINDOW_CLIENT_CONFIGURE: {
        if (!geometry(server, event)) _exit(1);
        BroadwayInputMsg msg = {.screen_resize_notify = {
            .base.type = BROADWAY_EVENT_SCREEN_SIZE_CHANGED, .width = event.x, .height = event.y}};
        dispatch(server, &msg);
        GdkBroadwayDisplay *display = GDK_BROADWAY_DISPLAY(gdk_display_get_default());
        GdkWindow *window = g_hash_table_lookup(display->id_ht, GINT_TO_POINTER(server->primary));
        if (window) gdk_window_move_resize(window, 0, 0, event.x, event.y);
        break;
    }
    case WINDOW_CLIENT_POINTER: {
        int old = server->hover;
        server->x = event.x;
        server->y = event.y;
        Surface *s = hit(server, event.x, event.y);
        server->hover = s ? s->id : 0;
        int id = server->grab ? server->grab : server->hover;
        if (old != server->hover) {
            pointer_event(server, BROADWAY_EVENT_LEAVE, old, 0);
            pointer_event(server, BROADWAY_EVENT_ENTER, server->hover, 0);
        }
        pointer_event(server, BROADWAY_EVENT_POINTER_MOVE, id, 0);
        gboolean down = event.value != 0;
        if (down != server->down) {
            if (down && id) _gdk_broadway_server_window_focus(server, id);
            pointer_event(server, down ? BROADWAY_EVENT_BUTTON_PRESS : BROADWAY_EVENT_BUTTON_RELEASE, id, 1);
            server->down = down;
        }
        break;
    }
    case WINDOW_CLIENT_WHEEL:
        pointer_event(server, BROADWAY_EVENT_SCROLL, server->hover, event.value < 0);
        break;
    case WINDOW_CLIENT_RIGHT_CLICK: {
        server->x = event.x;
        server->y = event.y;
        Surface *s = hit(server, event.x, event.y);
        if (s) {
            server->hover = s->id;
            _gdk_broadway_server_window_focus(server, s->id);
            pointer_event(server, BROADWAY_EVENT_BUTTON_PRESS, s->id, 3);
            pointer_event(server, BROADWAY_EVENT_BUTTON_RELEASE, s->id, 3);
        }
        break;
    }
    case WINDOW_CLIENT_KEY:
        keyboard(server, event.value);
        break;
    case WINDOW_CLIENT_TIMER:
        if (server->escape_size && g_get_monotonic_time() - server->escape_time >= 45000) {
            if (server->escape_size == 1) key_event(server, GDK_KEY_Escape, 0);
            server->escape_size = 0;
        }
        break;
    case WINDOW_CLIENT_CLOSE: {
        BroadwayInputMsg msg = {.delete_notify = {.base.type = BROADWAY_EVENT_DELETE_NOTIFY, .id = server->primary}};
        dispatch(server, &msg);
        break;
    }
    case WINDOW_CLIENT_PASTE: {
        char text[WINDOW_CLIENT_CLIPBOARD_MAX + 1];
        if (event.value < 1 || event.value >= sizeof text || !transfer(fd, text, event.value)) _exit(1);
        text[event.value] = 0;
        if (!g_utf8_validate(text, event.value, NULL)) break;
        for (const char *p = text; *p; p = g_utf8_next_char(p)) {
            gunichar ch = g_utf8_get_char(p);
            key_event(server, ch == '\n' ? GDK_KEY_Return : ch == '\t' ? GDK_KEY_Tab :
                      gdk_unicode_to_keyval(ch), 0);
        }
        break;
    }
    }
    /* Desktop sends the next input only after this reply. Ordinary GTK idle
     * work can stay ready indefinitely, so a protocol reply must outrank it. */
    server->reply_source = g_timeout_add(20, reply, server);
    return G_SOURCE_CONTINUE;
}

GdkBroadwayServer *_gdk_broadway_server_new(const char *name, GError **error)
{
    GdkBroadwayServer *server = g_object_new(GDK_TYPE_BROADWAY_SERVER, NULL);
    WindowEvent config;
    if (isatty(STDIN_FILENO) || !transfer(STDIN_FILENO, &config, sizeof config) ||
        config.type != WINDOW_CLIENT_CONFIGURE || !geometry(server, config)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Start this GTK application from Desktop Apps");
        g_object_unref(server);
        return NULL;
    }
    server->read_source = g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP, read_event, server);
    server->reply_source = g_timeout_add(20, reply, server);
    return server;
}

guint32 _gdk_broadway_server_new_window(GdkBroadwayServer *server, int x, int y, int w, int h, gboolean temp)
{
    Surface *s = g_new0(Surface, 1);
    *s = (Surface){.id = server->next_id++, .x = x, .y = y, .w = w, .h = h, .popup = temp};
    if (!server->primary && !temp) {
        server->primary = s->id;
        s->x = s->y = 0;
    }
    g_ptr_array_add(server->windows, s);
    return s->id;
}

void _gdk_broadway_server_destroy_window(GdkBroadwayServer *server, gint id)
{
    g_ptr_array_remove(server->windows, find(server, id));
    if (server->primary == id) server->primary = 0;
    if (server->focused == id) _gdk_broadway_server_window_focus(server, server->primary);
    if (server->grab == id) server->grab = 0;
    server->dirty = TRUE;
}

void _gdk_broadway_server_window_focus(GdkBroadwayServer *server, gint id)
{
    if (server->focused == id) return;
    BroadwayInputMsg msg = {.focus = {.base.type = BROADWAY_EVENT_FOCUS,
        .new_id = id, .old_id = server->focused}};
    server->focused = id;
    dispatch(server, &msg);
}

gboolean _gdk_broadway_server_window_show(GdkBroadwayServer *server, gint id)
{
    Surface *s = find(server, id);
    if (!s || s->visible) return FALSE;
    if (s->transient && !s->popup)
        _gdk_broadway_server_window_move_resize(server, id, TRUE, s->x, s->y, s->w, s->h);
    s->visible = TRUE;
    server->dirty = TRUE;
    _gdk_broadway_server_window_focus(server, id);
    return TRUE;
}

gboolean _gdk_broadway_server_window_hide(GdkBroadwayServer *server, gint id)
{
    Surface *s = find(server, id);
    if (!s || !s->visible) return FALSE;
    s->visible = FALSE;
    if (server->focused == id) _gdk_broadway_server_window_focus(server, server->primary);
    server->dirty = TRUE;
    return TRUE;
}

gboolean _gdk_broadway_server_window_move_resize(GdkBroadwayServer *server, gint id, gboolean move, int x, int y, int w, int h)
{
    Surface *s = find(server, id);
    if (!s) return FALSE;
    x = move ? (id == server->primary ? 0 : MAX(0, MIN(x, server->width - w))) : s->x;
    y = move ? (id == server->primary ? 0 : MAX(0, MIN(y, server->height - h))) : s->y;
    if (s->transient && !s->popup) {
        double scale = surface_scale(server, &(Surface){.w = w, .h = h});
        x = MAX(0, (server->width - w * scale) / 2);
        y = MAX(0, (server->height - h * scale) / 2);
    }
    if (x == s->x && y == s->y && w == s->w && h == s->h) return TRUE;
    s->x = x;
    s->y = y;
    s->w = w;
    s->h = h;
    server->dirty = TRUE;
    if (!s->configure) {
        s->configure = TRUE;
        g_idle_add_full(G_PRIORITY_DEFAULT, configure_windows, g_object_ref(server), g_object_unref);
    }
    return TRUE;
}

cairo_surface_t *_gdk_broadway_server_create_surface(int w, int h)
{
    return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
}

void _gdk_broadway_server_window_update(GdkBroadwayServer *server, gint id, cairo_surface_t *surface)
{
    Surface *s = find(server, id);
    if (!s || !surface) return;
    cairo_surface_reference(surface);
    if (s->surface) cairo_surface_destroy(s->surface);
    s->surface = surface;
    server->dirty = TRUE;
}

void _gdk_broadway_server_query_mouse(GdkBroadwayServer *server, guint32 *top, gint32 *x, gint32 *y, guint32 *mask)
{
    if (top) *top = server->hover;
    if (x) *x = server->x;
    if (y) *y = server->y;
    if (mask) *mask = server->down ? GDK_BUTTON1_MASK : 0;
}
GdkGrabStatus _gdk_broadway_server_grab_pointer(GdkBroadwayServer *server, gint id, gboolean owner, guint32 mask, guint32 time)
{
    server->grab = id;
    return GDK_GRAB_SUCCESS;
}
guint32 _gdk_broadway_server_ungrab_pointer(GdkBroadwayServer *server, guint32 time)
{
    server->grab = 0;
    return ++server->serial;
}
gint32 _gdk_broadway_server_get_mouse_toplevel(GdkBroadwayServer *server) { return server->hover; }
guint32 _gdk_broadway_server_get_last_seen_time(GdkBroadwayServer *server) { return server->time; }
gulong _gdk_broadway_server_get_next_serial(GdkBroadwayServer *server) { return server->serial + 1; }
gboolean _gdk_broadway_server_lookahead_event(GdkBroadwayServer *server, const char *types) { return FALSE; }
void _gdk_broadway_server_flush(GdkBroadwayServer *server) { }
void _gdk_broadway_server_sync(GdkBroadwayServer *server) { }
void _gdk_broadway_server_set_show_keyboard(GdkBroadwayServer *server, gboolean show) { }
void _gdk_broadway_server_window_set_transient_for(GdkBroadwayServer *server, gint id, gint parent)
{
    Surface *s = find(server, id);
    if (s) s->transient = parent;
}
void _gdk_broadway_server_window_set_modal_hint(GdkBroadwayServer *server, gint id, gboolean modal) { }
gboolean _gdk_broadway_server_window_translate(GdkBroadwayServer *server, gint id, cairo_region_t *area, gint dx, gint dy) { return FALSE; }
