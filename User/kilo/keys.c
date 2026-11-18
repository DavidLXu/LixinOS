/* Included by kilo.c; see its BSD license and copyright notice. */
/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch(key) {
    case ARROW_LEFT:
        if (E.cx == 0) {
            if (E.coloff) {
                E.coloff--;
            } else {
                if (filerow > 0) {
                    if (E.cy) E.cy--; else E.rowoff--;
                    E.cx = E.row[filerow-1].size;
                    if (E.cx > E.screencols-1) {
                        E.coloff = E.cx-E.screencols+1;
                        E.cx = E.screencols-1;
                    }
                }
            }
        } else {
            E.cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E.cx == E.screencols-1) {
                E.coloff++;
            } else {
                E.cx += 1;
            }
        } else if (row && filecol == row->size && filerow+1 < E.numrows) {
            E.cx = 0;
            E.coloff = 0;
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    case ARROW_UP:
        if (E.cy == 0) {
            if (E.rowoff) E.rowoff--;
        } else {
            E.cy -= 1;
        }
        break;
    case ARROW_DOWN:
        if (filerow+1 < E.numrows) {
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff+E.cy;
    filecol = E.coloff+E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol-rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define KILO_QUIT_TIMES 3
void editorProcessKeypress(int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey(fd);
    switch(c) {
    case ENTER:         /* Enter */
        editorInsertNewline();
        break;
    case CTRL_C:        /* Ctrl-c */
        /* We ignore ctrl-c, it can't be so simple to lose the changes
         * to the edited file. */
        break;
    case CTRL_Q:        /* Ctrl-q */
        /* Quit if the file was already saved. */
        if (E.dirty && quit_times) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        _exit(0);
        break;
    case CTRL_S:        /* Ctrl-s */
        editorSave();
        break;
    case CTRL_F:
        editorFind(fd);
        break;
    case BACKSPACE:     /* Backspace */
    case CTRL_H:        /* Ctrl-h */
        editorDelChar();
        break;
    case DEL_KEY:
        {
            int row = E.rowoff+E.cy;
            if (row < E.numrows && (E.coloff+E.cx < E.row[row].size || row+1 < E.numrows)) {
                editorMoveCursor(ARROW_RIGHT);
                editorDelChar();
            }
        }
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        if (c == PAGE_UP && E.cy != 0)
            E.cy = 0;
        else if (c == PAGE_DOWN && E.cy != E.screenrows-1)
            E.cy = E.screenrows-1;
        {
        int times = E.screenrows;
        while(times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP:
                                            ARROW_DOWN);
        }
        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    case HOME_KEY: E.cx = E.coloff = 0; break;
    case END_KEY: {
        int i = E.rowoff + E.cy;
        int end = i < E.numrows ? E.row[i].size : 0;
        E.cx = end < E.screencols ? end : E.screencols-1;
        E.coloff = end - E.cx;
        break;
    }
    case CTRL_L: /* ctrl+l, clear screen */
        /* Just refresht the line as side effect. */
        break;
    case ESC:
        /* Nothing to do for ESC in this mode. */
        break;
    default:
        if (c == TAB || (c >= 32 && c <= 126)) editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES; /* Reset it to the original value. */
}

void kilo(const char *name, char *text, size_t len) {
    unsigned long geometry = screen_info();
    E.screenrows = ((geometry >> 16) & 65535) - 2;
    E.screencols = geometry & 65535;
    editorSelectSyntaxHighlight((char *)name);
    editorOpen(name,text,len);
    if (!(geometry >> 32)) screen_write("\033[?1049h",8);
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(0);
    }
}
