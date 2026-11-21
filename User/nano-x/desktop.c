#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nano-X.h"
#include "tcc/lixin.h"
#include "gui/lixin-gui.h"

enum { TOOLBAR_HEIGHT = 44, FILE_ROW = 24, FILE_TOP = 74, ROWS = 11 };
static const GR_COLOR desktop_color = GR_RGB(23, 39, 55);
static const GR_COLOR toolbar_color = GR_RGB(35, 54, 73);
static const GR_COLOR paper = GR_RGB(244, 247, 249);
static const GR_COLOR ink = GR_RGB(26, 42, 56);
static const GR_COLOR muted = GR_RGB(82, 103, 123);
static const GR_COLOR light = GR_RGB(235, 241, 247);

struct desktop {
    GR_WINDOW_ID toolbar, files;
    GR_GC_ID gc;
    GR_SIZE width, file_width, file_height;
    unsigned offset;
    char directory[513];
};

int lx_nx_flush(void);

static void fill(GR_WINDOW_ID wid, GR_GC_ID gc, int x, int y, int w, int h, GR_COLOR color) {
    GrSetGCForeground(gc, color);
    GrFillRect(wid, gc, x, y, w, h);
}

static void text(GR_WINDOW_ID wid, GR_GC_ID gc, int x, int y, const char *label, GR_COLOR color) {
    GrSetGCForeground(gc, color);
    GrText(wid, gc, x, y, (void *)label, (int)strlen(label), GR_TFASCII | GR_TFTOP);
}

static void paint_toolbar(const struct desktop *desk) {
    GR_WINDOW_ID wid = desk->toolbar;
    GR_GC_ID gc = desk->gc;
    fill(wid, gc, 12, 8, 74, 28, GR_RGB(68, 120, 159));
    text(wid, gc, 30, 15, "Files", light);
    text(wid, gc, 102, 15, "Nano-X", light);
    fill(wid, gc, desk->width - 76, 8, 64, 28, GR_RGB(64, 83, 102));
    text(wid, gc, desk->width - 56, 15, "Exit", light);
}

static void paint_files(const struct desktop *desk) {
    GR_WINDOW_ID wid = desk->files;
    GR_GC_ID gc = desk->gc;
    fill(wid, gc, 0, 0, desk->file_width, desk->file_height, paper);
    fill(wid, gc, 12, 12, 52, 26, GR_RGB(221, 231, 239));
    text(wid, gc, 28, 19, "Up", ink);
    text(wid, gc, 78, 19, desk->directory, ink);
    text(wid, gc, 16, 50, "Name", muted);
    text(wid, gc, desk->file_width - 76, 50, "Type", muted);
    for (unsigned i = 0; i < ROWS; i++) {
        char name[513];
        long size = lx_dir(desk->directory, desk->offset + i, name);
        if (size != LX_DIR && size < 0)
            break;
        int y = FILE_TOP + (int)i * FILE_ROW;
        fill(wid, gc, 12, y, desk->file_width - 24, FILE_ROW - 1,
             i % 2 ? paper : GR_RGB(232, 239, 245));
        name[42] = '\0';
        text(wid, gc, 18, y + 4, name, ink);
        text(wid, gc, desk->file_width - 76, y + 4, size == LX_DIR ? "Folder" : "File", muted);
    }
}

static void open_files(struct desktop *desk) {
    if (desk->files) {
        GrRaiseWindow(desk->files);
        return;
    }
    GR_WM_PROPS policy = GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE | GR_WM_PROPS_NOAUTOMOVE;
    desk->files = GrNewWindowEx(policy, "Files", GR_ROOT_WINDOW_ID,
                                32, TOOLBAR_HEIGHT + 24, desk->file_width, desk->file_height, paper);
    GrSelectEvents(desk->files, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN |
                   GR_EVENT_MASK_CLOSE_REQ);
    GrMapWindow(desk->files);
}

static void navigate(struct desktop *desk, int x, int y, int buttons) {
    if (buttons & GR_BUTTON_SCROLLUP) {
        if (desk->offset)
            desk->offset--;
    } else if (buttons & GR_BUTTON_SCROLLDN) {
        char name[513];
        long size = lx_dir(desk->directory, desk->offset + ROWS, name);
        if (size == LX_DIR || size >= 0)
            desk->offset++;
    } else if (buttons & GR_BUTTON_L) {
        if (x >= 12 && x < 64 && y >= 12 && y < 38) {
            char *last = strrchr(desk->directory, '/');
            (last == desk->directory ? last + 1 : last)[0] = '\0';
            desk->offset = 0;
        } else if (y >= FILE_TOP && y < FILE_TOP + ROWS * FILE_ROW) {
            char name[513];
            unsigned index = desk->offset + (y - FILE_TOP) / FILE_ROW;
            if (lx_dir(desk->directory, index, name) != LX_DIR)
                return;
            size_t length = strlen(desk->directory);
            const char *separator = length == 1 ? "" : "/";
            if (length + strlen(separator) + strlen(name) >= sizeof(desk->directory))
                return;
            strcat(desk->directory, separator);
            strcat(desk->directory, name);
            desk->offset = 0;
        }
    }
    paint_files(desk);
}

int main(void) {
    if (GrOpen() < 0) {
        fputs("nano-x: a foreground QEMU canvas is required\n", stderr);
        return 1;
    }
    GR_SCREEN_INFO screen;
    GrGetScreenInfo(&screen);
    if (screen.cols < 440 || screen.rows < 430) {
        fputs("nano-x: canvas too small\n", stderr);
        return 1;
    }
    struct desktop desk = {
        .gc = GrNewGC(),
        .width = screen.cols,
        .file_width = 390,
        .file_height = FILE_TOP + ROWS * FILE_ROW + 8,
        .directory = "/",
    };
    if (!getcwd(desk.directory, sizeof(desk.directory)))
        strcpy(desk.directory, "/");
    GrSetGCUseBackground(desk.gc, GR_FALSE);
    GR_WM_PROPERTIES root = { .flags = GR_WM_FLAGS_BACKGROUND, .background = desktop_color };
    GrSetWMProperties(GR_ROOT_WINDOW_ID, &root);
    GrClearWindow(GR_ROOT_WINDOW_ID, GR_FALSE);
    desk.toolbar = GrNewWindowEx(GR_WM_PROPS_NODECORATE, NULL, GR_ROOT_WINDOW_ID,
                                  0, 0, desk.width, TOOLBAR_HEIGHT, toolbar_color);
    GrSelectEvents(desk.toolbar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN);
    GrMapWindow(desk.toolbar);

    int quit = 0, pointer_visible = 0, x = screen.cols / 2, y = screen.rows / 2;
    while (!quit) {
        struct lx_pointer pointer;
        for (unsigned i = 0; i < 64 && lx_gui_pointer(&pointer); i++) {
            int inside = pointer.inside && !pointer.cancelled;
            int visibility = inside == pointer_visible ? 0 : (inside ? 1 : 2);
            pointer_visible = inside;
            if (inside) {
                x = pointer.x;
                y = pointer.y;
            }
            int buttons = inside && pointer.down ? GR_BUTTON_L : 0;
            if (inside && pointer.wheel) {
                int wheel = pointer.wheel > 0 ? GR_BUTTON_SCROLLUP : GR_BUTTON_SCROLLDN;
                GrInjectPointerEvent(x, y, buttons | wheel, visibility);
                visibility = 0;
            }
            GrInjectPointerEvent(x, y, buttons, visibility);
        }
        GR_EVENT event;
        for (unsigned i = 0; i < 64 && GrPeekEvent(&event); i++) {
            GrCheckNextEvent(&event);
            if (event.type == GR_EVENT_TYPE_EXPOSURE) {
                if (event.exposure.wid == desk.toolbar) paint_toolbar(&desk);
                else if (event.exposure.wid == desk.files) paint_files(&desk);
            } else if (event.type == GR_EVENT_TYPE_CLOSE_REQ && event.general.wid == desk.files) {
                GrDestroyWindow(desk.files);
                desk.files = 0;
            } else if (event.type == GR_EVENT_TYPE_BUTTON_DOWN) {
                if (event.button.wid == desk.toolbar && (event.button.buttons & GR_BUTTON_L)) {
                    if (event.button.x >= desk.width - 76) quit = 1;
                    else if (event.button.x >= 12 && event.button.x < 86) open_files(&desk);
                } else if (event.button.wid == desk.files) {
                    navigate(&desk, event.button.x, event.button.y, event.button.buttons);
                }
            }
        }
        if (lx_nx_flush() != 0) {
            fputs("nano-x: screen update failed\n", stderr);
            return 1;
        }
        if (!quit && lx_timed_byte(14) == 27)
            quit = 1;
    }
    GrClose();
    return 0;
}
