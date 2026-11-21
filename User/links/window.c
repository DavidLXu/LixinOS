#include "window.h"
#include <string.h>
#include <stdlib.h>
#include "unicode.h"
#include "../gui/han-font.h"
extern const unsigned char desktop_font[];
enum { PAPER=0xffffff, INK=0x172e32, TOOL=0xe7efee, LINK=0x185e93, WALL=0x26494c };
static void rect(struct browser_window *w,int x,int y,int width,int height,unsigned c) {
    lx_gui_rect(&w->canvas,w->x+x,w->y+y,width,height,c);
}
static void glyph(struct browser_window *w,int x,int y,unsigned ch,unsigned color,unsigned bg) {
    if (ch==WIDE_TAIL) return;
    const unsigned char *bits=lx_han_glyph(ch);
    if (bits && x+18<=w->w) {
        rect(w,x,y,18,22,bg);
        for (int j=0;j<16;j++) for (int i=0;i<16;i++)
            if (bits[j*2+i/8] & (128>>(i%8)))
                w->canvas.pixels[(w->y+y+j+3)*w->canvas.width+w->x+x+i+1]=color;
        return;
    }
    if (ch<32 || ch>126) ch='?';
    const unsigned char *a=desktop_font+(ch-32)*18*22;
    for (int j=0;j<22;j++) for (int i=0;i<9;i++) {
        unsigned alpha=a[j*18+i];
        unsigned rb=(((bg&0xff00ff)*(256-alpha)+(color&0xff00ff)*alpha)>>8)&0xff00ff;
        unsigned g=(((bg&0xff00)*(256-alpha)+(color&0xff00)*alpha)>>8)&0xff00;
        w->canvas.pixels[(w->y+y+j)*w->canvas.width+w->x+x+i]=rb|g;
    }
}
static void label(struct browser_window *w,int x,int y,int width,const char *s,unsigned fg,unsigned bg) {
    while (*s && width>=9) {
        unsigned c=utf8_next(&s);
        int advance=han_slot(c)>=0?18:9;
        if (advance>width) break;
        glyph(w,x,y,c,fg,bg); x+=advance; width-=advance;
    }
}
int window_open(struct browser_window *w) {
    if (lx_gui_open(&w->canvas)<0) return 0;
    w->x=12; w->y=12; w->w=w->canvas.width-24; w->h=w->canvas.height-24;
    return 1;
}
void window_frame(struct browser_window *w,const char *title,const char *address,const char *status) {
    if (!w->hosted) lx_gui_rect(&w->canvas,0,0,w->canvas.width,w->canvas.height,WALL);
    rect(w,0,0,w->w,w->h,PAPER);
    rect(w,0,0,w->w,44,INK);
    label(w,14,11,60,"Links",PAPER,INK);
    label(w,82,11,w->w-135,title,0xc7ddde,INK);
    rect(w,w->w-35,8,28,28,TOOL);
    label(w,w->w-25,11,12,"x",INK,TOOL);
    rect(w,0,44,w->w,34,TOOL);
    label(w,12,50,55,"< Back",INK,TOOL);
    rect(w,80,48,w->w-90,26,PAPER);
    label(w,88,50,w->w-102,address,INK,PAPER);
    rect(w,0,w->h-30,w->w,30,TOOL);
    label(w,12,w->h-26,w->w-24,status,INK,TOOL);
}
void window_char(struct browser_window *w,int col,int row,unsigned ch,int link) {
    int x=WEB_MARGIN+col*WEB_CHAR, y=WEB_TOP+row*WEB_LINE;
    glyph(w,x,y,ch,link?LINK:INK,link==2?0xdcebfa:PAPER);
    if (link) rect(w,x,y+19,WEB_CHAR,1,LINK);
}
void window_present(struct browser_window *w) {
    if (w->hosted) return;
    if (w->inside) for (int i=0;i<13;i++) {
        lx_gui_rect(&w->canvas,w->mouse_x,w->mouse_y+i,i/2+1,1,INK);
        if (i>3) lx_gui_rect(&w->canvas,w->mouse_x+1,w->mouse_y+i,i/2-1,1,PAPER);
    }
    lx_gui_present(&w->canvas);
}
