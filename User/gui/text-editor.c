#include "lixin-gui.h"
#include <lixin.h>
#include "text-clipboard.h"
#include "file-dialog.h"
#include <stdio.h>
#include <string.h>

enum { LIMIT = 32768, LEFT = 28, TOP = 91 };
enum { KEY_LEFT = 300, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_DELETE };
static struct lx_font font;
static struct lx_canvas canvas;
static struct lx_gui_cursor cursor;
static struct lx_button save, close, open_button, save_as;
static UiControls toolbar;
static FileDialog dialog;
static char document_path[513];
static int confirm_open;
static char text[LIMIT + 1];
static unsigned starts[LIMIT + 2];
static unsigned length, caret, anchor, first, rows, columns, lines;
static int dirty, quitting, read_only, selecting;
static const char *path, *status = "Ready";

/* Return one valid scalar's byte length. Never step past an incomplete sequence. */
static int scalar_bytes(const char *p,unsigned left) {
    unsigned c=(unsigned char)*p;
    if (c<128) return (c>=32 && c!=127) || c=='\n' || c=='\t' ? 1 : 0;
    int n=c<194 || c>244 ? 0 : c<224 ? 2 : c<240 ? 3 : 4;
    if (!n || (unsigned)n>left) return 0;
    unsigned second=(unsigned char)p[1];
    if ((c==224 && second<160) || (c==237 && second>=160) ||
        (c==240 && second<144) || (c==244 && second>=144)) return 0;
    for (int i=1;i<n;++i) if (((unsigned char)p[i]&192)!=128) return 0;
    return n;
}
static unsigned cells_at(unsigned at) {
    const char *p=text+at;
    return han_slot(utf8_next(&p))>=0 ? 2 : 1;
}
static unsigned cells_between(unsigned begin,unsigned end) {
    unsigned col=0;
    while (begin<end) { col+=cells_at(begin); begin=ui_next(text,begin); }
    return col;
}
static unsigned position_at(unsigned begin,unsigned end,unsigned pixel) {
    while (begin<end) {
        unsigned width=9*cells_at(begin);
        if (pixel<width/2) break;
        begin=ui_next(text,begin);
        if (pixel<width) break;
        pixel-=width;
    }
    return begin;
}
static UiText decoder;
static char glyph[5];

static void layout(void) {
    lines = 1;
    starts[0] = 0;
    unsigned col = 0;
    for (unsigned i = 0; i < length; i=ui_next(text,i)) {
        if (text[i] == '\n') {
            starts[lines++] = i + 1;
            col = 0;
        } else {
            unsigned width=cells_at(i);
            if (col && col+width>columns) {
                starts[lines++] = i;
                col = 0;
            }
            col+=width;
        }
    }
    starts[lines] = length;
}

static unsigned row_end(unsigned row) {
    unsigned end = starts[row + 1];
    if (row + 1 < lines && end && text[end - 1] == '\n') --end;
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

static unsigned selection_start(void) { return anchor < caret ? anchor : caret; }
static unsigned selection_end(void) { return anchor > caret ? anchor : caret; }

static int over_text(const struct lx_pointer *p) {
    return p->inside && p->x >= 16 && p->x < canvas.width - 16 &&
           p->y >= TOP && p->y < TOP + rows * 22;
}

static unsigned character_at(const struct lx_pointer *p) {
    unsigned y = p->y < TOP ? 0 : (p->y - TOP) / 22;
    if (y >= rows) y = rows - 1;
    unsigned row = first + y;
    if (row >= lines) return length;
    return position_at(starts[row],row_end(row),p->x>LEFT ? p->x-LEFT : 0);
}

static void render(void) {
    if (dialog.active) {
        fd_draw(&dialog,&canvas,&font);
        lx_gui_present(&canvas); lx_gui_cursor_show(&canvas,&cursor); return;
    }
    lx_gui_rect(&canvas, 0, 0, canvas.width, canvas.height, 0xe7ece8);
    lx_gui_rect(&canvas, 0, 0, canvas.width, 65, 0x233b43);
    lx_gui_text(&canvas, &font, LEFT, 7, 17, "Text Editor", 0xf3faf3);
    fd_label(&canvas, &font, LEFT, 33, canvas.width-(canvas.width>=650 ? 450 : 260), path, 0xb8d6cf);
    if (canvas.width>=650) {
        lx_gui_button(&canvas,&font,&open_button,0x405b65,0xffffff);
        lx_gui_button(&canvas,&font,&save_as,0x405b65,0xffffff);
    }
    lx_gui_button(&canvas, &font, &save, read_only ? 0x535f63 : 0x24736f, 0xffffff);
    lx_gui_button(&canvas, &font, &close, 0x405b65, 0xffffff);
    lx_gui_rect(&canvas, 16, 76, canvas.width-32, canvas.height-118, 0xfcfcf7);
    unsigned lo = selection_start(), hi = selection_end();
    for (unsigned row = first; row < lines && row < first + rows; ++row) {
        unsigned from=starts[row], to=row_end(row);
        unsigned start=lo>from?lo:from, end=hi<to?hi:to;
        if (end>start)
            lx_gui_rect(&canvas,LEFT+9*cells_between(from,start),TOP+(row-first)*22,
                        9*cells_between(start,end),22,0xb2d7f3);
        /* Temporarily terminate at the visual row boundary for the shared glyph renderer. */
        char tail=text[to]; text[to]=0;
        fd_label(&canvas,&font,LEFT,TOP+(row-first)*22,columns*9,text+from,0x283e45);
        text[to]=tail;
    }
    unsigned row = caret_row();
    if (row >= first && row < first + rows)
        lx_gui_rect(&canvas, LEFT+cells_between(starts[row],caret)*9, TOP+(row-first)*22, 2, 20, 0x168178);
    lx_gui_text(&canvas, &font, LEFT, canvas.height-32, columns,
                quitting ? "Unsaved changes. Esc again to discard, Ctrl+S to save."
                : status, 0x38525a);
    lx_gui_present(&canvas);
    lx_gui_cursor_show(&canvas, &cursor);
}

static int load_document(const char *target,int allow_new) {
    static char incoming[LIMIT+1];
    unsigned size=0;
    int fd=lx_open(target);
    if (fd<0 && (!allow_new || lx_stat(target)!=LX_MISSING)) return 0;
    if (fd>=0) {
        while (size<sizeof incoming) {
            int n=lx_read(fd,incoming+size,sizeof incoming-size);
            if (n<0) { lx_close(fd); return 0; }
            if (!n) break;
            size+=n;
        }
        lx_close(fd);
    }
    read_only=size>LIMIT;
    length=read_only ? LIMIT : size;
    status=read_only ? "Read only: exceeds 32 KiB." : fd<0 ? "New file. Ctrl+S to save." : "Ready";
    memcpy(text,incoming,length);
    text[length]=0;
    for (unsigned i=0;i<length;) {
        int n=scalar_bytes(text+i,length-i);
        if (!n) {
            read_only=1;
            status="Read only: invalid UTF-8 or unsupported control characters.";
            text[i++]='?';
        } else i+=n;
    }
    strcpy(document_path,target);
    caret=anchor=first=dirty=quitting=selecting=0;
    return 1;
}
static void choose_document(void) {
    if (dialog.save) {
        if (read_only || lx_save_file(dialog.result,text,length)<0) {
            dialog.hint="Save failed. Choose another location."; return;
        }
        strcpy(document_path,dialog.result); dirty=0; status="Saved.";
    } else if (load_document(dialog.result,0)) layout();
    else { dialog.hint="Open failed. Choose another file."; return; }
    dialog.active=0;
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

static int replace_selection(const char *bytes,unsigned n,unsigned lo,unsigned hi) {
    if (length-(hi-lo)+n>LIMIT) { status="Document limit is 32 KiB."; return 0; }
    memmove(text+lo+n,text+hi,length-hi+1);
    if (n) memcpy(text+lo,bytes,n);
    length=length-(hi-lo)+n;
    caret=anchor=lo+n;
    dirty=1; status="Modified. Ctrl+S to save.";
    layout(); reveal();
    return 1;
}
static int edit(int key) {
    if (key<128 || key>255) decoder.pending=0;
    if (dialog.active) {
        if ((key==3 || key==24) && dialog.controls.focus==FD_NAME) {
            int a=dialog.edit.anchor,b=dialog.edit.caret;
            if (a!=b) {
                lx_copy_text(dialog.name+(a<b?a:b),a<b?b-a:a-b);
                if (key==24) fd_key(&dialog,&canvas,8);
            }
        } else if (fd_key(&dialog,&canvas,key)) choose_document();
        return 0;
    }
    if (key==15 || key==20) {
        if (key==15 && dirty && !confirm_open) {
            confirm_open=1; status="Unsaved changes. Ctrl+O again to select a replacement, or Ctrl+S to save.";
            return 0;
        }
        confirm_open=0;
        if (key==15 || !read_only) fd_begin(&dialog,path,key==20);
        return 0;
    }
    confirm_open=0;
    if (key == 27) {
        if (!dirty || quitting) return 1;
        quitting = 1;
        return 0;
    }
    quitting = 0;
    unsigned row = caret_row(), col = cells_between(starts[row],caret);
    if (key == 19) {
        if (!read_only && lx_save_file(path, text, length) == 0) {
            dirty = 0; status = "Saved.";
        } else if (!read_only) status = "Save failed. Your changes are still in memory.";
    } else if (key == 1) {
        anchor = 0;
        caret = length;
        reveal();
        return 0;
    } else if (key == KEY_LEFT) caret=caret!=anchor ? selection_start() : (unsigned)ui_prev(text,caret);
    else if (key == KEY_RIGHT) caret=caret!=anchor ? selection_end() : (unsigned)ui_next(text,caret);
    else if (key == KEY_HOME) caret = starts[row];
    else if (key == KEY_END) caret = row_end(row);
    else if ((key == KEY_UP && row) || (key == KEY_DOWN && row + 1 < lines)) {
        row = key == KEY_UP ? row - 1 : row + 1;
        caret=position_at(starts[row],row_end(row),col*9);
    } else if (key==3 || key==24) {
        unsigned lo=selection_start(),hi=selection_end();
        if (hi-lo>4096) { status="Clipboard limit is 4096 bytes."; return 0; }
        if (hi>lo) {
            lx_copy_text(text+lo,hi-lo);
            if (key==24 && !read_only) replace_selection("",0,lo,hi);
        }
        return 0;
    } else if (!read_only) {
        unsigned lo=selection_start(),hi=selection_end();
        if (key==8 || key==127 || key==KEY_DELETE) {
            if (lo==hi) {
                if (key==KEY_DELETE) hi=ui_next(text,hi);
                else lo=ui_prev(text,lo);
            }
            if (lo!=hi) replace_selection("",0,lo,hi);
        } else if (key==13 || key==10 || key==9) {
            replace_selection(key==9?"\t":"\n",1,lo,hi);
        } else if (key>=32 && key<=255) {
            glyph[0]=0; decoder.caret=decoder.anchor=0;
            if (ui_text_key(&decoder,glyph,sizeof glyph,key)) replace_selection(glyph,strlen(glyph),lo,hi);
        }
        return 0;
    }
    if (key != 19) anchor = caret;
    reveal();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) { puts("usage: text-editor [path]"); return 2; }
    path=document_path;
    const char *initial=argc==2 ? argv[1] : "/notes.txt";
    if (strlen(initial)>512) return 2;
    if (!load_document(initial,1)) {
        strcpy(document_path,initial); read_only=1; status="Unable to open this path.";
    }
    if (lx_gui_font(&font) || lx_gui_open(&canvas)) return 1;
    rows = (canvas.height-137)/22;
    columns = (canvas.width-2*LEFT-9)/9;
    save = (struct lx_button){canvas.width-206, 14, 90, 37, "Save"};
    close = (struct lx_button){canvas.width-108, 14, 90, 37, "Close"};
    if (canvas.width>=650) {
        open_button=(struct lx_button){canvas.width-400,14,84,37,"Open"};
        save_as=(struct lx_button){canvas.width-308,14,94,37,"Save As"};
    }
    layout();
    render();
    int held = 0;
    for (;;) {
        int key = key_read(), redraw = key != 256;
        if (redraw && edit(key)) return 0;
        struct lx_pointer pointer;
        for (;;) {
            long options = (read_only ? 0 : LX_PASTE_MULTILINE) | (anchor == caret ? 0 : LX_COPY_ENABLED);
            if (dialog.active) {
                options=LX_PASTE_BLOCK | (dialog.edit.caret!=dialog.edit.anchor ? LX_COPY_ENABLED : 0);
                long n=lx_console_call(35,0);
                if (n>0 && n<=4096 && dialog.controls.focus==FD_NAME) {
                    redraw |= ui_text_paste(&dialog.edit,dialog.name,sizeof dialog.name,
                        (const unsigned char *)0x443d8000UL,n);
                    dialog.confirm[0]=0; dialog.selected=-1;
                }
            }
            if (!dialog.active && !read_only) {
                options|=LX_PASTE_BLOCK;
                long n=lx_console_call(35,0);
                if (n>0 && n<=4096) {
                    const char *bytes=(const char *)0x443d8000UL;
                    int i=0;
                    while (i<n) {
                        int k=scalar_bytes(bytes+i,n-i);
                        if (!k) break;
                        i+=k;
                    }
                    if (i==n) replace_selection(bytes,n,selection_start(),selection_end());
                    else status="Paste contains invalid text.";
                    decoder.pending=0; redraw=1;
                }
            }
            if (!lx_gui_decode_pointer(&pointer, lx_console_call(21, options))) break;
            if (dialog.active) {
                if (pointer.copy) {
                    int a=dialog.edit.anchor,b=dialog.edit.caret;
                    lx_copy_text(dialog.name+(a<b?a:b),a<b?b-a:a-b);
                } else if (fd_pointer(&dialog,&canvas,&pointer)) choose_document();
                held=pointer.down; redraw=1;
                lx_gui_cursor_move(&canvas,&cursor,&pointer); continue;
            }
            int hit=lx_gui_hit(&close,&pointer)?1:lx_gui_hit(&save,&pointer)?2:
                lx_gui_hit(&open_button,&pointer)?3:lx_gui_hit(&save_as,&pointer)?4:0;
            int action=ui_pointer(&toolbar,pointer.cancelled?0:hit,pointer.down && !pointer.cancelled);
            if (action) {
                if (edit(action==1?27:action==2?19:action==3?15:20)) return 0;
                held=0; selecting=0; redraw=1; continue;
            }
            if (pointer.copy) {
                unsigned lo = selection_start();
                lx_copy_text(text + lo, selection_end() - lo);
            } else if (pointer.cancelled) {
                held = selecting = 0;
            } else if (pointer.down && !held) {
                if (over_text(&pointer)) {
                    caret = anchor = character_at(&pointer);
                    selecting = 1;
                }
                redraw = 1;
            } else if (selecting) {
                if (pointer.down || pointer.inside) caret = character_at(&pointer);
                selecting = pointer.down;
                redraw = 1;
            } else if (pointer.inside && pointer.wheel) {
                if (pointer.wheel > 0 && first) --first;
                if (pointer.wheel < 0 && first + rows < lines) ++first;
                redraw = 1;
            }
            held = pointer.down;
            lx_gui_cursor_move_as(&canvas, &cursor, &pointer, over_text(&pointer));
        }
        if (redraw) render();
    }
}
