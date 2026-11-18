/* Included by kilo.c; see its BSD license and copyright notice. */
/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = resize_or_exit(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

static int visualColumn(const erow *row, int index) {
    int column = 0;
    for (int i=0; i<index && i<row->size; i++)
        column += row->chars[i] == TAB ? 8 - column%8 : 1;
    return column;
}

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;
    int active = E.rowoff + E.cy;
    int visual = active < E.numrows ? visualColumn(&E.row[active], E.coloff+E.cx) : 0;
    int left = active < E.numrows ? visualColumn(&E.row[active], E.coloff) : 0;
    if (visual-left >= E.screencols) left = visual-E.screencols+1;

    abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
    abAppend(&ab,"\x1b[H",3); /* Go home. */
    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff+y;

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows/3) {
                char welcome[80];
                int welcomelen = snprintf(welcome,sizeof(welcome),
                    "Kilo editor -- version %s\x1b[0K\r\n", KILO_VERSION);
                int padding = (E.screencols-welcomelen)/2;
                if (padding) {
                    abAppend(&ab,"~",1);
                    padding--;
                }
                while(padding--) abAppend(&ab," ",1);
                abAppend(&ab,welcome,welcomelen);
            } else {
                abAppend(&ab,"~\x1b[0K\r\n",7);
            }
            continue;
        }

        r = &E.row[filerow];

        int len = r->rsize - left;
        int current_color = -1;
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render+left;
            unsigned char *hl = r->hl+left;
            int j;
            for (j = 0; j < len; j++) {
                if ((unsigned char)c[j] < 32 || c[j] == 127 || hl[j] == HL_NONPRINT) {
                    char sym;
                    abAppend(&ab,"\x1b[7m",4);
                    if (c[j] <= 26)
                        sym = '@'+c[j];
                    else
                        sym = '?';
                    abAppend(&ab,&sym,1);
                    abAppend(&ab,"\x1b[0m",4);
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab,"\x1b[39m",5);
                        current_color = -1;
                    }
                    abAppend(&ab,c+j,1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                        current_color = color;
                        abAppend(&ab,buf,clen);
                    }
                    abAppend(&ab,c+j,1);
                }
            }
        }
        abAppend(&ab,"\x1b[39m",5);
        abAppend(&ab,"\x1b[0K",4);
        abAppend(&ab,"\r\n",2);
    }

    /* Create a two rows status. First row: */
    abAppend(&ab,"\x1b[0K",4);
    abAppend(&ab,"\x1b[7m",4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
        "%d/%d",E.rowoff+E.cy+1,E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(&ab,status,len);
    while(len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab,rstatus,rlen);
            break;
        } else {
            abAppend(&ab," ",1);
            len++;
        }
    }
    abAppend(&ab,"\x1b[0m\r\n",6);

    /* Second row depends on E.statusmsg and the status message update time. */
    abAppend(&ab,"\x1b[0K",4);
    int msglen = strlen(E.statusmsg);
    if (msglen)
        abAppend(&ab,E.statusmsg,msglen <= E.screencols ? msglen : E.screencols);

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy+1,visual-left+1);
    abAppend(&ab,buf,strlen(buf));
    abAppend(&ab,"\x1b[?25h",6); /* Show cursor. */
    screen_write(ab.b,ab.len);
    abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);

}

/* =============================== Find mode ================================ */

#define KILO_QUERY_LEN 256

void editorFind(int fd) {
    char query[KILO_QUERY_LEN+1] = {0};
    int qlen = 0;
    int last_match = -1; /* Last line where a match was found. -1 for none. */
    int find_next = 0; /* if 1 search next, if -1 search prev. */
    int saved_hl_line = -1;  /* No saved HL */
    char *saved_hl = NULL;

#define FIND_RESTORE_HL do { \
    if (saved_hl) { \
        memcpy(E.row[saved_hl_line].hl,saved_hl, E.row[saved_hl_line].rsize); \
        free(saved_hl); \
        saved_hl = NULL; \
    } \
} while (0)

    /* Save the cursor position in order to restore it later. */
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    while(1) {
        editorSetStatusMessage(
            "Search: %s (Use ESC/Arrows/Enter)", query);
        editorRefreshScreen();

        int c = editorReadKey(fd);
        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            last_match = -1;
        } else if (c == ESC || c == ENTER) {
            if (c == ESC) {
                E.cx = saved_cx; E.cy = saved_cy;
                E.coloff = saved_coloff; E.rowoff = saved_rowoff;
            }
            FIND_RESTORE_HL;
            editorSetStatusMessage("");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < KILO_QUERY_LEN) {
                query[qlen++] = c;
                query[qlen] = '\0';
                last_match = -1;
            }
        }

        /* Search occurrence. */
        if (last_match == -1) find_next = 1;
        if (find_next) {
            char *match = NULL;
            int match_offset = 0;
            int i, current = last_match;

            for (i = 0; i < E.numrows; i++) {
                current += find_next;
                if (current == -1) current = E.numrows-1;
                else if (current == E.numrows) current = 0;
                match = strstr(E.row[current].render,query);
                if (match) {
                    match_offset = match-E.row[current].render;
                    break;
                }
            }
            find_next = 0;

            /* Highlight */
            FIND_RESTORE_HL;

            if (match) {
                erow *row = &E.row[current];
                last_match = current;
                if (row->hl) {
                    saved_hl_line = current;
                    saved_hl = resize_or_exit(NULL,row->rsize);
                    memcpy(saved_hl,row->hl,row->rsize);
                    memset(row->hl+match_offset,HL_MATCH,qlen);
                }
                E.cy = 0;
                E.cx = 0;
                for (int col=0; col < match_offset && E.cx < row->size; E.cx++)
                    col += row->chars[E.cx] == TAB ? 8-col%8 : 1;
                E.rowoff = current;
                E.coloff = 0;
                /* Scroll horizontally as needed. */
                if (E.cx >= E.screencols) {
                    int diff = E.cx - E.screencols + 1;
                    E.cx -= diff;
                    E.coloff += diff;
                }
            }
        }
    }
}
