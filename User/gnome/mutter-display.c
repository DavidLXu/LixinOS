#include "config.h"

#include <GLES2/gl2.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "backends/lixin-display.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-stage-private.h"
#include "clutter/clutter-mutter.h"

typedef struct {
    struct lx_canvas canvas;
    ClutterVirtualInputDevice *pointer;
    ClutterVirtualInputDevice *keyboard;
    guint input_source;
    char escape[12];
    unsigned escape_size;
    gint64 escape_time;
    int last_x, last_y;
    gboolean left_button;
    gboolean needs_full_frame;
    gboolean trace_frames;
    gboolean reported_renderer;
} LxMutterDisplay;

static void present_frame(MetaStage *stage, ClutterStageView *view,
                          const MtkRegion *clip, ClutterFrame *frame, gpointer data) {
    LxMutterDisplay *display = data;
    MtkRectangle layout;
    clutter_stage_view_get_layout(view, &layout);
    if (layout.x != 0 || layout.y != 0 || layout.width != display->canvas.width ||
        layout.height != display->canvas.height || clutter_stage_view_get_scale(view) != 1.f)
        return;

    gboolean full_frame = display->needs_full_frame || !clip;
    int count = full_frame ? 1 : mtk_region_num_rectangles(clip);
    unsigned submitted = 0;
    for (int i = 0; i < count; ++i) {
        MtkRectangle rect = full_frame ? layout : mtk_region_get_rectangle(clip, i);
        if (!mtk_rectangle_intersect(&layout, &rect, &rect))
            continue;

        /* The canvas syscall takes tightly packed rows. Reuse its arena as a
         * tile buffer, including for the first frame and unrestricted redraws. */
        clutter_stage_capture_view_into(CLUTTER_STAGE(stage), view, &rect,
                                        (uint8_t *)display->canvas.pixels, rect.width * 4);
        if (lx_canvas_present(display->canvas.pixels, rect.x, rect.y,
                              rect.width, rect.height) < 0) {
            display->needs_full_frame = TRUE;
            return;
        }
        submitted += rect.width * rect.height;
    }
    if (submitted && !display->reported_renderer) {
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        if (renderer) {
            g_print("LixinOS compositor renderer: %s\n", renderer);
            display->reported_renderer = TRUE;
        }
    }
    display->needs_full_frame = FALSE;
    if (display->trace_frames)
        g_print("LixinOS frame pixels %u/%u\n", submitted, display->canvas.width * display->canvas.height);
}

static void send_pointer(LxMutterDisplay *display, const struct lx_pointer *pointer, uint64_t now) {
    if (pointer->inside && !pointer->cancelled &&
        (pointer->x != display->last_x || pointer->y != display->last_y)) {
        display->last_x = pointer->x;
        display->last_y = pointer->y;
        clutter_virtual_input_device_notify_absolute_motion(display->pointer, now,
                                                            pointer->x, pointer->y);
    }
    gboolean down = pointer->down && !pointer->cancelled && (pointer->inside || display->left_button);
    if (down != display->left_button) {
        display->left_button = down;
        clutter_virtual_input_device_notify_button(display->pointer, now, 1,
            down ? CLUTTER_BUTTON_STATE_PRESSED : CLUTTER_BUTTON_STATE_RELEASED);
    }
    if (pointer->inside && !pointer->cancelled && pointer->wheel)
        clutter_virtual_input_device_notify_discrete_scroll(display->pointer, now,
            pointer->wheel > 0 ? CLUTTER_SCROLL_UP : CLUTTER_SCROLL_DOWN, CLUTTER_SCROLL_SOURCE_WHEEL);
}

static uint32_t to_keysym(int byte) {
    switch (byte) {
    case 127: return XKB_KEY_BackSpace;
    case 9: return XKB_KEY_Tab;
    case 10:
    case 13: return XKB_KEY_Return;
    case 27: return XKB_KEY_Escape;
    default: return (byte >= 32 && byte < 127) ? (uint32_t)byte : 0;
    }
}

static void send_key(LxMutterDisplay *display, uint64_t now, uint32_t key, gboolean ctrl) {
    if (ctrl)
        clutter_virtual_input_device_notify_keyval(display->keyboard, now, XKB_KEY_Control_L, CLUTTER_KEY_STATE_PRESSED);
    clutter_virtual_input_device_notify_keyval(display->keyboard, now, key, CLUTTER_KEY_STATE_PRESSED);
    clutter_virtual_input_device_notify_keyval(display->keyboard, now, key, CLUTTER_KEY_STATE_RELEASED);
    if (ctrl)
        clutter_virtual_input_device_notify_keyval(display->keyboard, now, XKB_KEY_Control_L, CLUTTER_KEY_STATE_RELEASED);
}

static void keyboard_byte(LxMutterDisplay *display, uint64_t now, int byte) {
    static const struct { const char *seq; uint32_t key; } navigation[] = {
        {"[A", XKB_KEY_Up}, {"[B", XKB_KEY_Down}, {"[C", XKB_KEY_Right}, {"[D", XKB_KEY_Left},
        {"[H", XKB_KEY_Home}, {"[F", XKB_KEY_End}, {"[2~", XKB_KEY_Insert}, {"[3~", XKB_KEY_Delete},
        {"[5~", XKB_KEY_Page_Up}, {"[6~", XKB_KEY_Page_Down},
        {"[15~", XKB_KEY_F5}, {"[17~", XKB_KEY_F6}, {"[18~", XKB_KEY_F7},
        {"[19~", XKB_KEY_F8}, {"[20~", XKB_KEY_F9}
    };
    if (display->escape_size) {
        if (display->escape_size > 1 || byte == '[' || byte == 'O') {
            if (display->escape_size < sizeof(display->escape) - 1) {
                display->escape[display->escape_size++] = byte;
                display->escape[display->escape_size] = 0;
                if (display->escape_size == 2 || (byte >= '0' && byte <= '9') || byte == ';')
                    return;
                // Application cursor mode sends SS3 in place of CSI.
                if (display->escape[1] == 'O') display->escape[1] = '[';
                for (unsigned i = 0; i < G_N_ELEMENTS(navigation); i++)
                    if (!strcmp(display->escape + 1, navigation[i].seq)) {
                        send_key(display, now, navigation[i].key, FALSE);
                        break;
                    }
            }
            display->escape_size = 0;
            return;
        }
        display->escape_size = 0;
        send_key(display, now, XKB_KEY_Escape, FALSE);
    }
    if (byte == 27) {
        display->escape_size = 1;
        display->escape_time = now;
        return;
    }
    uint32_t key = to_keysym(byte);
    if (key)
        send_key(display, now, key, FALSE);
    else if (byte >= 1 && byte <= 26)
        send_key(display, now, XKB_KEY_a + byte - 1, TRUE);
}

static gboolean dispatch_input(gpointer data) {
    LxMutterDisplay *display = data;
    uint64_t now = g_get_monotonic_time();
    struct lx_pointer pointer;
    for (unsigned i = 0; i < 16 && lx_gui_pointer(&pointer); ++i)
        send_pointer(display, &pointer, now);
    for (unsigned i = 0; i < 16; ++i) {
        int byte = lx_timed_byte(0);
        if (byte < 0 || byte > 255)
            break;
        keyboard_byte(display, now, byte);
    }
    if (display->escape_size && now - display->escape_time > 50000) {
        if (display->escape_size == 1)
            send_key(display, now, XKB_KEY_Escape, FALSE);
        display->escape_size = 0;
    }
    return G_SOURCE_CONTINUE;
}

static void close_display(gpointer data) {
    LxMutterDisplay *display = data;
    g_source_remove(display->input_source);
    g_object_unref(display->pointer);
    g_object_unref(display->keyboard);
    g_free(display);
}

void lx_mutter_attach_display(MetaBackend *backend, const struct lx_canvas *canvas) {
    MetaStage *stage = META_STAGE(meta_backend_get_stage(backend));
    ClutterSeat *seat = meta_backend_get_default_seat(backend);
    LxMutterDisplay *display = g_new0(LxMutterDisplay, 1);
    display->canvas = *canvas;
    display->needs_full_frame = TRUE;
    display->trace_frames = g_getenv("LIXINOS_TRACE_FRAMES") != NULL;
    display->last_x = display->last_y = -1;
    display->pointer = clutter_seat_create_virtual_device(seat, CLUTTER_POINTER_DEVICE);
    display->keyboard = clutter_seat_create_virtual_device(seat, CLUTTER_KEYBOARD_DEVICE);
    display->input_source = g_timeout_add(16, dispatch_input, display);
    g_object_set_data_full(G_OBJECT(stage), "lixinos-display", display, close_display);
    meta_stage_watch_view(stage, NULL, META_STAGE_WATCH_AFTER_PAINT, present_frame, display);
}
