#ifndef LX_WINDOW_WIDGETS_H
#define LX_WINDOW_WIDGETS_H
#include "window-app.h"
#include "../gui/widgets.h"
#ifdef __TINYC__
#include "../gui/han-font.c"
#else
#include "../gui/han-font.h"
#include "../links/unicode.h"
#endif
static inline int advance(unsigned cp) { return han_slot(cp)>=0 ? 16 : 11; }

/* Draw only complete glyphs, including at narrow sizes and on long paths. */
static inline void label(AppWindow *s,int x,int y,int width,const char *p,unsigned short ink) {
    while (*p && width>0) {
        unsigned cp=utf8_next(&p);
        int step=advance(cp);
        if (step>width) break;
        if (step==16) {
            const unsigned char *alpha=lx_pingfang_glyph(cp,1);
            const unsigned char *bits=alpha ? NULL : lx_han_glyph(cp);
            for (int j=0;j<16 && y+j+3<s->height;++j) {
                for (int i=0;i<16 && x+i<s->width;++i) {
                    if (y+j+3<0 || x+i<0) continue;
                    unsigned a=alpha ? alpha[j*16+i] : bits && (bits[j*2+i/8] & (128>>(i%8))) ? 255 : 0;
                    if (!a) continue;
                    unsigned short *pix=&s->pixels[(y+j+3)*s->width+x+i];
                    unsigned bg=*pix, b=255-a;
                    *pix=((((ink>>11)*a+(bg>>11)*b)/255)<<11) |
                         (((((ink>>5)&63)*a+((bg>>5)&63)*b)/255)<<5) |
                         (((ink&31)*a+(bg&31)*b)/255);
                }
            }
        } else {
            char ascii[2]={cp<127 ? (char)cp : '?',0};
            app_text(s,x,y,ascii,ink);
        }
        x+=step; width-=step;
    }
}

typedef struct { int id,x,y,w,h; const char *name; } AppControl;
static inline int app_control_hit(AppControl b,int x,int y) {
    return x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h;
}
static inline void app_button(AppWindow *s,UiControls *controls,AppControl b,int primary) {
    unsigned short fill=controls->pressed==b.id ? 0x29aa : primary ? 0x1b8d : 0xdedb;
    app_rect(s,b.x,b.y,b.w,b.h,fill);
    label(s,b.x+6,b.y+1,b.w-9,b.name,primary ? 0xffff : 0x1947);
    if (controls->focus==b.id) app_rect(s,b.x+3,b.y+b.h-3,b.w-6,2,0x42ca);
}
static inline int app_text_width(const char *s,int start,int end) {
    const char *p=s+start;
    int w=0;
    while (p<s+end) w+=advance(utf8_next(&p));
    return w;
}
static inline int app_field_offset(const UiText *t,const char *s,int room) {
    int start=t->caret,used=0;
    while (start) {
        int prev=ui_prev(s,start);
        used+=app_text_width(s,prev,start);
        if (used>room) break;
        start=prev;
    }
    return start;
}
static inline int app_field_position(AppControl b,UiText *t,const char *s,int x) {
    int i=app_field_offset(t,s,b.w-12),local=x-b.x-5;
    while (s[i]) {
        int next=ui_next(s,i),w=app_text_width(s,i,next);
        if (local<w/2) break;
        local-=w; i=next;
    }
    return i;
}
static inline void app_field(AppWindow *win,AppControl b,const UiText *t,const char *s,int focused) {
    int room=b.w-12,start=app_field_offset(t,s,room);
    int lo=t->anchor<t->caret?t->anchor:t->caret,hi=t->anchor+t->caret-lo;
    app_rect(win,b.x,b.y,b.w,b.h,0xffff);
    if (focused && hi>start && hi>lo) {
        int left=app_text_width(s,start,lo>start?lo:start);
        int right=ui_limit(app_text_width(s,start,hi),room);
        if (right>left) app_rect(win,b.x+5+left,b.y+2,right-left,b.h-4,0xd730);
    }
    label(win,b.x+5,b.y+2,room,s+start,0x1947);
    if (focused) {
        app_rect(win,b.x,b.y+b.h-1,b.w,1,0x1b8d);
        app_rect(win,b.x+5+app_text_width(s,start,t->caret),b.y+5,1,b.h-10,0x1b8d);
    }
}
static inline int app_field_paste(AppWindow *win,UiText *t,char *s,int capacity,int length) {
    return ui_text_paste(t,s,capacity,win->paste,length);
}
static inline int app_field_key(AppWindow *win,UiText *t,char *s,int capacity,int key) {
    if (key==3 || key==24) {
        int lo=t->anchor<t->caret?t->anchor:t->caret, n=t->anchor+t->caret-2*lo;
        static char copy[WINDOW_CLIENT_CLIPBOARD_MAX];
        if (!n || n>(int)sizeof copy) return 0;
        memcpy(copy,s+lo,n); win->copy=copy; win->copy_size=n;
        if (key==3) return 0;
        key=8;
    }
    return ui_text_key(t,s,capacity,key);
}
#endif
