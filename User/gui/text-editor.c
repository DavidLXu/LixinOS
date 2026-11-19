#include <lixin-gui.h>
#include <lixin.h>
#include <stdio.h>
#include <string.h>

enum { LIMIT = 32768, LEFT = 28, TOP = 91 };
enum { KEY_LEFT = 300, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_DELETE };
static struct lx_font font;
static struct lx_canvas canvas;
static struct lx_gui_cursor cursor;
static struct lx_button save, close;
static char text[LIMIT + 1];
static unsigned starts[LIMIT + 2];
static unsigned length, caret, first, rows, columns, lines;
static int dirty, quitting, read_only;
static const char *path, *status = "Ready";

static void layout(void) {
    lines = 1;
    starts[0] = 0;
    unsigned col = 0;
    for (unsigned i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            starts[lines++] = i + 1;
            col = 0;
        } else {
            if (col == columns) {
                starts[lines++] = i;
                col = 0;
            }
            ++col;
        }
    }
    starts[lines] = length;
}

static unsigned row_end(unsigned row) {
    unsigned end = starts[row + 1];
    if (row + 1 < lines && end && text[end - 1] == '\n') --end;
    else if (row + 1 < lines) --end;
    return end;
}

static unsigned caret_row(void) {
    unsigned lo = 0, hi = lines;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (starts[mid] <= caret) lo = mid;
        else hi = mid;
    }
    return lo;
}

static void reveal(void) {
    unsigned row = caret_row();
    if (row < first) first = row;
    if (row >= first + rows) first = row - rows + 1;
    if (first >= lines) first = lines - 1;
}

static void render(void) {
    lx_gui_rect(&canvas, 0, 0, canvas.width, canvas.height, 0xe7ece8);
    lx_gui_rect(&canvas, 0, 0, canvas.width, 65, 0x233b43);
    lx_gui_text(&canvas, &font, LEFT, 7, 17, "Text Editor", 0xf3faf3);
    lx_gui_text(&canvas, &font, LEFT, 33, (canvas.width-260)/9, path, 0xb8d6cf);
    lx_gui_button(&canvas, &font, &save, read_only ? 0x535f63 : 0x24736f, 0xffffff);
    lx_gui_button(&canvas, &font, &close, 0x405b65, 0xffffff);
    lx_gui_rect(&canvas, 16, 76, canvas.width-32, canvas.height-118, 0xfcfcf7);
    for (unsigned row = first; row < lines && row < first + rows; ++row) {
        char line[256];
        unsigned from = starts[row], to = starts[row + 1];
        if (to > from && text[to - 1] == '\n') --to;
        for (unsigned i = from; i < to; ++i)
            line[i - from] = text[i] == '\t' ? ' ' : text[i];
        line[to - from] = 0;
        lx_gui_text(&canvas, &font, LEFT, TOP+(row-first)*22, columns, line, 0x283e45);
    }
    unsigned row = caret_row();
    if (row >= first && row < first + rows)
        lx_gui_rect(&canvas, LEFT+(caret-starts[row])*9, TOP+(row-first)*22, 2, 20, 0x168178);
    lx_gui_text(&canvas, &font, LEFT, canvas.height-32, columns,
                quitting ? "Unsaved changes. Esc again to discard, Ctrl+S to save."
                : status, 0x38525a);
    lx_gui_present(&canvas);
    lx_gui_cursor_show(&canvas, &cursor);
}

static void load(void) {
    int fd = lx_open(path);
    if (fd < 0) {
        long size = lx_stat(path);
        if (size != LX_MISSING) { read_only = 1; status = "Unable to open this path."; }
        else status = "New file. Ctrl+S to save.";
        return;
    }
    while (length <= LIMIT) {
        int n = lx_read(fd, text + length, sizeof(text)-length);
        if (n == 0) break;
        if (n < 0) { read_only = 1; status = "Read failed."; break; }
        length += n;
    }
    lx_close(fd);
    if (length > LIMIT) { read_only = 1; length = LIMIT; status = "Read only: exceeds 32 KiB."; }
    for (unsigned i = 0; i < length; ++i) {
        unsigned char c = text[i];
        if (c != '\n' && c != '\t' && (c < 32 || c > 126)) {
            read_only = 1;
            status = "Read only: this editor supports ASCII text with LF line endings.";
            if (!c) text[i] = '?';
        }
    }
}

static int key_read(void) {
    int key = lx_timed_byte(12);
    if (key != 27) return key;
    key = lx_timed_byte(30);
    if (key != '[' && key != 'O') return 27;
    switch (lx_timed_byte(30)) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '3': return lx_timed_byte(30) == '~' ? KEY_DELETE : 256;
    default: return 256;
    }
}

static int edit(int key) {
    if (key == 27) {
        if (!dirty || quitting) return 1;
        quitting = 1;
        return 0;
    }
    quitting = 0;
    unsigned row = caret_row(), col = caret - starts[row];
    if (key == 19) {
        if (!read_only && lx_save_file(path, text, length) == 0) {
            dirty = 0; status = "Saved.";
        } else if (!read_only) status = "Save failed. Your changes are still in memory.";
    } else if (key == KEY_LEFT && caret) --caret;
    else if (key == KEY_RIGHT && caret < length) ++caret;
    else if (key == KEY_HOME) caret = starts[row];
    else if (key == KEY_END) caret = row_end(row);
    else if ((key == KEY_UP && row) || (key == KEY_DOWN && row + 1 < lines)) {
        row = key == KEY_UP ? row - 1 : row + 1;
        caret = starts[row] + col;
        if (caret > row_end(row)) caret = row_end(row);
    } else if (!read_only) {
        if (((key == 8 || key == 127) && caret) || (key == KEY_DELETE && caret < length)) {
            if (key != KEY_DELETE) --caret;
            memmove(text + caret, text + caret + 1, length - caret - 1);
            --length;
        } else if ((key >= 32 && key <= 126) || key == 13 || key == 10 || key == 9) {
            if (length == LIMIT) { status = "Document limit is 32 KiB."; return 0; }
            memmove(text + caret + 1, text + caret, length - caret);
            text[caret++] = key == 13 ? '\n' : key;
            ++length;
        } else return 0;
        dirty = 1;
        status = "Modified. Ctrl+S to save.";
        layout();
    }
    reveal();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) { puts("usage: text-editor [path]"); return 2; }
    path = argc == 2 ? argv[1] : "/notes.txt";
    load();
    if (lx_gui_font(&font) || lx_gui_open(&canvas)) return 1;
    rows = (canvas.height-137)/22;
    columns = (canvas.width-2*LEFT-9)/9;
    save = (struct lx_button){canvas.width-206, 14, 90, 37, "Save"};
    close = (struct lx_button){canvas.width-108, 14, 90, 37, "Close"};
    layout();
    render();
    int held = 0;
    for (;;) {
        int key = key_read(), redraw = key != 256;
        if (redraw && edit(key)) return 0;
        struct lx_pointer pointer;
        while (lx_gui_pointer(&pointer)) {
            if (pointer.cancelled) held = 0;
            else if (pointer.down && !held) {
                if (lx_gui_hit(&close, &pointer)) { if (edit(27)) return 0; }
                else if (lx_gui_hit(&save, &pointer)) edit(19);
                else if (pointer.inside && pointer.x >= LEFT && pointer.y >= TOP
                         && pointer.y < TOP + rows*22) {
                    unsigned row = first + (pointer.y-TOP)/22;
                    if (row < lines) {
                        caret = starts[row] + (pointer.x-LEFT)/9;
                        if (caret > row_end(row)) caret = row_end(row);
                    }
                }
                redraw = 1;
            } else if (pointer.inside && pointer.wheel) {
                if (pointer.wheel > 0 && first) --first;
                if (pointer.wheel < 0 && first + rows < lines) ++first;
                redraw = 1;
            }
            held = pointer.down;
            lx_gui_cursor_move(&canvas, &cursor, &pointer);
        }
        if (redraw) render();
    }
}
