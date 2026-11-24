#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../console.h"
#include "../tcc/lixin.h"
#include "../gui/lixin-gui.h"
#include "../gui/mouse-cursor.h"
#include "../gui/text-clipboard.h"
#include "../desktop/window-client-host.h"
#include "session.h"

enum { TITLE_HEIGHT = 26 };
typedef struct {
    struct lx_canvas canvas;
    unsigned generation;
    WindowClient clients[MAX_CLIENTS + 1]; /* slot zero is the Xfce panel */
    char names[MAX_CLIENTS + 1][513], cwd[513], status[100];
    int commands_fd, active, grab, down, quitting, repaint, mouse_x, mouse_y, inside;
    int closing_pid;
    long allow_force_after;
    struct lx_font font;
} Session;

static int minimum(int a, int b) { return a < b ? a : b; }

static void configure(Session *s, int i) {
    WindowClient *c = &s->clients[i];
    int top = i ? PANEL_HEIGHT + TITLE_HEIGHT : 0;
    if (c->adaptive)
        client_event(c, (WindowEvent){WINDOW_CLIENT_CONFIGURE, minimum(2048,s->canvas.width),
            minimum(1152, s->canvas.height - top), WINDOW_CLIENT_RIGHT_CLICK_AVAILABLE});
    else client_event(c, (WindowEvent){WINDOW_CLIENT_POINTER, 0, 0, 0});
}

static int geometry(Session *s) {
    long size = console_call(19, 1);
    if (size <= 0) return 0;
    unsigned generation = (unsigned long)size >> 32;
    if (s->generation != generation || !s->canvas.width) {
        s->generation = generation;
        s->canvas.width = size & 65535;
        s->canvas.height = (size >> 16) & 65535;
        if (s->canvas.height <= PANEL_HEIGHT + TITLE_HEIGHT) return 0;
        s->canvas.pixels = (unsigned *)0x68000000UL;
        for (int i = 0; i <= MAX_CLIENTS; ++i)
            if (s->clients[i].pid && !s->clients[i].stopping) configure(s, i);
        s->grab = -1;
        s->down = 0;
        s->repaint = 1;
    }
    return 1;
}

static void submit(Session *s, unsigned *pixels, int x, int y, int w, int h, int stride) {
    volatile unsigned long *m = (void *)0x443d8000UL;
    m[0]=(uintptr_t)pixels; m[1]=x; m[2]=y; m[3]=w; m[4]=h; m[5]=stride;
    console_call(20, s->generation);
}

static void cursor(Session *s, int draw) {
    int x = s->mouse_x, y = s->mouse_y;
    int width = s->canvas.width, height = s->canvas.height;
    if (!s->inside || x >= width || y >= height) return;
    int w = minimum(CUR_W, width-x), h = minimum(CUR_H, height-y);
    unsigned *tile = s->canvas.pixels + width*height;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            unsigned color = s->canvas.pixels[(y+j)*width+x+i];
            tile[j*w+i] = draw ? lixin_cursor_pixel(lixin_cursor[j][i], color) : color;
        }
    submit(s, tile, x, y, w, h, w);
}

static void surface(Session *s, WindowClient *c, int top, int height, int rows) {
    if (!c->ready || !c->pixels) return;
    int width = s->canvas.width;
    for (int y = 0; y < rows; ++y) {
        unsigned *dest = s->canvas.pixels + (top+y)*width;
        const unsigned short *src = c->pixels + (y*c->height/height)*c->width;
        for (int x = 0; x < width; ++x) {
            unsigned p = src[x*c->width/width];
            *dest++ = ((p & 0xf800)<<8) | ((p & 0x7e0)<<5) | ((p & 31)<<3);
        }
    }
}

static void paint(Session *s) {
    int w = s->canvas.width, h = s->canvas.height, top = PANEL_HEIGHT + TITLE_HEIGHT;
    lx_gui_rect(&s->canvas, 0, 0, w, h, 0xe6edf4);
    surface(s, &s->clients[0], 0, h, s->active ? PANEL_HEIGHT : h);
    if (s->active) {
        lx_gui_rect(&s->canvas, 0, PANEL_HEIGHT, w, TITLE_HEIGHT, 0x384d66);
        lx_gui_text(&s->canvas, &s->font, 12, PANEL_HEIGHT+2, 64, s->names[s->active], 0xffffff);
        surface(s, &s->clients[s->active], top, h-top, h-top);
    }
    if (s->status[0]) {
        lx_gui_rect(&s->canvas, 0, h-26, w, 26, 0x384d66);
        lx_gui_text(&s->canvas, &s->font, 12, h-24, 96, s->status, 0xffffff);
    }
    submit(s, s->canvas.pixels, 0, 0, w, h, w);
    cursor(s, 1);
    s->repaint = 0;
}

static int next(Session *s) {
    for (int k = 1; k <= MAX_CLIENTS; ++k) {
        int i = (s->active+k-1) % MAX_CLIENTS + 1;
        if (s->clients[i].pid && !s->clients[i].stopping) return i;
    }
    return 0;
}

static void open_app(Session *s, const char *cmd, const char *argument) {
    int empty = 0;
    for (int i = 1; i <= MAX_CLIENTS; ++i) {
        if (!argument && s->clients[i].pid && !s->clients[i].stopping && !strcmp(s->names[i],cmd+5)) {
            s->active = i;
            return;
        }
        if (!empty && !s->clients[i].pid) empty = i;
    }
    if (!empty) strcpy(s->status,"All application windows are in use.");
    else if (!client_open_streams(&s->clients[empty], s->cwd, cmd,
                                  !strcmp(cmd, "/bin/thunar.win3") ? "--lixin-session" : argument,
                                  !strcmp(cmd, "/bin/thunar.win3") ? s->commands_fd : 255))
        strcpy(s->status,"Could not start the selected application.");
    else {
        strcpy(s->names[empty], cmd+5);
        s->active = empty;
        configure(s, empty);
    }
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    return c >= 'a' && c <= 'f' ? c-'a'+10 : -1;
}

static void open_document(Session *s, const char *hex) {
    size_t n = strlen(hex);
    if (!n || n % 2 || n > 1024) return;
    char path[513];
    for (size_t i=0; i<n; i+=2) {
        int hi=nibble(hex[i]), lo=nibble(hex[i+1]);
        if (hi<0 || lo<0 || !(hi|lo)) return;
        path[i/2]=(hi<<4)|lo;
    }
    path[n/2]=0;
    if (path[0] != '/' || lx_stat(path) < 0) return;
    s->status[0]=0;
    s->repaint=1;
    open_app(s, "/bin/mousepad.win3", path);
}

static void command(Session *s, const char *cmd) {
    if (!strcmp(cmd, "close") && s->active && s->closing_pid == s->clients[s->active].pid &&
        console_call(38, 0) < s->allow_force_after) return;
    s->status[0] = 0;
    s->repaint = 1;
    if (strcmp(cmd, "close")) s->closing_pid = 0;
    if (!strcmp(cmd,"home")) s->active = 0;
    else if (!strcmp(cmd,"next")) s->active = next(s);
    else if (!strcmp(cmd,"close")) {
        if (s->active) {
            WindowClient *c = &s->clients[s->active];
            if (c->extended && s->closing_pid == c->pid) {
                client_close(c);
                s->closing_pid = 0;
                s->active = next(s);
                return;
            }
            if (c->extended) {
                s->closing_pid = c->pid;
                s->allow_force_after = console_call(38, 0) + 1200;
                strcpy(s->status, "Close requested. Click Close window again to force quit and discard unsaved changes.");
            }
            /* Mousepad's application quit action also handles unsaved documents. */
            if (!strcmp(s->names[s->active], "mousepad.win3"))
                client_event(c, (WindowEvent){WINDOW_CLIENT_KEY,0,0,17});
            else if (c->extended) client_event(c, (WindowEvent){WINDOW_CLIENT_CLOSE,0,0,0});
            else { client_close(c); s->active = next(s); }
        }
    } else if (!strcmp(cmd,"logout")) {
        if (next(s)) strcpy(s->status, "Close the open windows before logging out.");
        else s->quitting = 1;
    } else if (!strncmp(cmd,"/bin/",5) && strlen(cmd)<=512 && window_name(cmd+5)) {
        open_app(s,cmd,NULL);
    }
}

static void read_commands(Session *s, int fd) {
    static char line[1032];
    static size_t used;
    static int record;
    char data[256];
    while (lx_poll(fd, LX_READABLE, 0) > 0) {
        int n = lx_read(fd, data, sizeof data);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            if (data[i] == XFCE_RECORD[0]) {
                record = 1;
                used = 0;
            } else if (!record) {
                continue;
            } else if (data[i] == '\n') {
                if (used < sizeof line) {
                    line[used] = 0;
                    if (!strncmp(line,"XFCE ",5)) command(s,line+5);
                    else if (!strncmp(line,"EDIT ",5)) open_document(s,line+5);
                }
                record = 0;
            } else if (used < sizeof line) line[used++] = data[i];
        }
    }
}

static void pointer(Session *s, long e) {
    cursor(s, 0);
    s->mouse_x = e & 4095; s->mouse_y = (e>>12) & 4095;
    s->inside = !!(e & (1L<<25));
    int down = (e>>24)&1;
    int slot = s->grab >= 0 ? s->grab :
        (s->active && s->mouse_y >= PANEL_HEIGHT ? s->active : 0);
    WindowClient *c = &s->clients[slot];
    int top = slot ? PANEL_HEIGHT+TITLE_HEIGHT : 0;
    if (s->inside && (!slot || s->mouse_y >= top || s->grab >= 0)) {
        int w = c->adaptive ? c->target_w : WINDOW_CLIENT_WIDTH;
        int h = c->adaptive ? c->target_h : WINDOW_CLIENT_HEIGHT;
        int x = s->mouse_x*w/(int)s->canvas.width;
        int y = (s->mouse_y-top)*h/((int)s->canvas.height-top);
        int wheel = ((e>>26)&1)-((e>>27)&1);
        if (e & LX_POINTER_COPY) client_event(c,(WindowEvent){WINDOW_CLIENT_KEY,0,0,3});
        else if (e & LX_RIGHT_PRESS) {
            if (c->right_click) client_event(c,(WindowEvent){WINDOW_CLIENT_RIGHT_CLICK,x,y,1});
        } else if (wheel && c->extended) client_event(c,(WindowEvent){WINDOW_CLIENT_WHEEL,x,y,wheel});
        else client_event(c,(WindowEvent){WINDOW_CLIENT_POINTER,x,y,down});
        if (down && !s->down) s->grab = slot;
    }
    if ((e & (1L<<28)) || !s->inside) {
        if (s->grab >= 0)
            client_event(&s->clients[s->grab],(WindowEvent){WINDOW_CLIENT_POINTER,-1,-1,0});
        down = 0;
    }
    if (!down) s->grab = -1;
    s->down = down;
    cursor(s, 1);
}

int main(void) {
    static Session s;
    s.grab = -1;
    if (!geometry(&s)) { fputs("startxfce4 requires a graphical foreground console.\n",stderr); return 1; }
    if (!getcwd(s.cwd,sizeof s.cwd)) strcpy(s.cwd,"/");
    lx_gui_font(&s.font);
    long channel = console_call(49,0);
    if (channel < 0) return 1;
    int read_fd = channel >> 32, write_fd = channel & 255;
    int ok = client_open_streams(&s.clients[0],s.cwd,"/lib/xfce/panel.win3",NULL,write_fd);
    s.commands_fd = write_fd;
    if (!ok) { lx_close(write_fd); lx_close(read_fd); fputs("Could not start the Xfce panel.\n",stderr); return 1; }
    configure(&s,0);
    int failed = 0;
    while (!s.quitting && geometry(&s)) {
        for (int i = 0; i <= MAX_CLIENTS; ++i) {
            int was_ready = s.clients[i].ready;
            int changed = client_tick(&s.clients[i], i == 0 || i == s.active);
            /* GTK consumes the handshake before its first toplevel exists. Deliver
             * its actual allocation once the first surface has been created. */
            if (changed > 0 && !was_ready && s.clients[i].adaptive) configure(&s, i);
            if (changed < 0) {
                client_close(&s.clients[i]);
                if (!i) { failed = 1; s.quitting = 1; }
                else if (s.active == i) {
                    s.active = next(&s);
                    s.closing_pid = 0;
                    s.status[0] = 0;
                }
            }
            if (changed && (i == 0 || i == s.active || changed < 0)) s.repaint = 1;
        }
        read_commands(&s, read_fd);
        if (s.repaint) paint(&s);
        int key = console_call(14, 2);
        if (key >= 0 && key <= 255 && s.closing_pid) { s.closing_pid = 0; s.status[0] = 0; s.repaint = 1; }
        if (key == 29) command(&s,"next");
        else if (key >= 0 && key <= 255)
            client_event(&s.clients[s.active], (WindowEvent){WINDOW_CLIENT_KEY,0,0,key});
        if (s.clients[s.active].extended) client_collect_paste(&s.clients[s.active]);
        long event;
        while ((event = console_call(21,LX_CONTEXT_CUSTOM|LX_COPY_ENABLED|LX_PASTE_BLOCK)) >= 0)
            pointer(&s,event);
    }
    for (int i = 0; i <= MAX_CLIENTS; ++i) client_shutdown(&s.clients[i]);
    lx_close(write_fd);
    lx_close(read_fd);
    if (failed) fputs("Xfce panel exited unexpectedly.\n",stderr);
    return failed;
}
