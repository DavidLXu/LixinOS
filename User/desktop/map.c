#include "window-app.h"
#include "../console.h"
#include <stdio.h>
#include <string.h>

enum { TILE = 256, SLOTS = 64, BAR = 26, FOOT = 20, MAX_ZOOM = 19 };
typedef struct { unsigned short colors[256]; unsigned char pixels[TILE*TILE]; } TileImage;
typedef struct { int z,x,y,ready; unsigned age; TileImage *image; } Tile;
static Tile cache[SLOTS];
static unsigned age;
static int zoom=10, cx=219523, cy=107113;
static int down, dragging, last_x, last_y, fd=-1, received, pending;
static long deadline, retry;
static int error;
static int wrap(int p,int size) { p%=size; return p<0?p+size:p; }
static int index_of(int x,int y) {
    for(int i=0;i<SLOTS;i++)
        if(cache[i].ready && cache[i].z==zoom && cache[i].x==x && cache[i].y==y) return i;
    return -1;
}
static void close_fetch(void) {
    if(fd>=0) lx_tcp_close(fd);
    fd=-1;
}
static void failed(long now) {
    close_fetch();error=1;retry=now+5000;
}
static void request(int x,int y,long now) {
    int i=0;
    for(int k=0;k<SLOTS;k++) {
        if(!cache[k].ready) {i=k;break;}
        if(cache[k].age<cache[i].age) i=k;
    }
    Tile *t=&cache[i];
    if(!t->image) t->image=realloc(0,sizeof(TileImage));
    if(!t->image) {failed(now);return;}
    fd=lx_tcp_connect("10.0.2.101",8080);
    if(fd<0) {failed(now);return;}
    char message[40];
    int len=snprintf(message,sizeof message,"%d %d %d\n",zoom,x,y);
    if(lx_tcp_write(fd,message,len)!=len) {failed(now);return;}
    pending=i;t->ready=0;t->z=zoom;t->x=x;t->y=y;
    received=0;deadline=now+15000;
}
/* Return the nearest missing visible tile, prioritizing the view center. */
static int next_tile(AppWindow *s,int *tx,int *ty) {
    int size=1<<zoom,w=s->width,h=s->height-BAR-FOOT,best=0x7fffffff;
    if(h<1) return 0;
    int left=cx-w/2,top=cy-h/2;
    for(int y=top>>8;y<=((top+h-1)>>8);y++) {
        if(y<0 || y>=size) continue;
        for(int x=left>>8;x<=((left+w-1)>>8);x++) {
            int col=wrap(x,size),dx=x*256+128-cx,dy=y*256+128-cy;
            int distance=dx*dx+dy*dy;
            if(distance<best && index_of(col,y)<0) {best=distance;*tx=col;*ty=y;}
        }
    }
    return best!=0x7fffffff;
}
static int poll_map(AppWindow *s) {
    long now=console_call(38,0);
    if(fd>=0) {
        Tile *t=&cache[pending];
        while(received<(int)sizeof(TileImage)) {
            int n=lx_tcp_read(fd,(char*)t->image+received,sizeof(TileImage)-received);
            if(n==LX_TCP_AGAIN && now<deadline) return 0;
            if(n<=0) {failed(now);return 1;}
            received+=n;
        }
        close_fetch();t->ready=1;t->age=++age;error=0;
        return 1;
    }
    int x,y;
    if(!dragging && now>=retry && next_tile(s,&x,&y)) {
        request(x,y,now);return 1;
    }
    return 0;
}
static int event(AppWindow *s,WindowEvent e) {
    int delta=0,changed=0;
    if(e.type==WINDOW_CLIENT_POINTER) {
        if(e.value && !down) {
            dragging=e.y>=BAR && e.y<s->height-FOOT;
            if(e.y<BAR && e.x>=s->width-72) delta=e.x<s->width-36?-1:1;
        }
        if(e.value && down && dragging) {
            cx+=last_x-e.x;cy+=last_y-e.y;changed=1;
        }
        if(!e.value) dragging=0;
        down=e.value;last_x=e.x;last_y=e.y;
    }
    if(e.type==WINDOW_CLIENT_KEY) delta=(e.value=='+' || e.value=='=')-(e.value=='-');
    if(delta && zoom+delta>=0 && zoom+delta<=MAX_ZOOM) {
        if(delta>0) {cx*=2;cy*=2;} else {cx/=2;cy/=2;}
        zoom+=delta;close_fetch();retry=0;changed=1;
    }
    int world=TILE<<zoom;
    cx=wrap(cx,world);
    if(cy<0) cy=0;
    if(cy>=world) cy=world-1;
    if(e.type==WINDOW_CLIENT_TIMER || e.type==WINDOW_CLIENT_CONFIGURE) changed|=poll_map(s);
    s->timer_ms=fd>=0?40:200;
    return changed;
}
static void draw(AppWindow *s) {
    int w=s->width,h=s->height-BAR-FOOT,left=cx-w/2,top=cy-h/2;
    app_rect(s,0,0,w,s->height,0xc658);
    for(int y=top>>8;y<=((top+h-1)>>8);y++) {
        if(y<0 || y>=(1<<zoom)) continue;
        for(int x=left>>8;x<=((left+w-1)>>8);x++) {
            int i=index_of(wrap(x,1<<zoom),y);
            if(i<0) continue;
            Tile *t=&cache[i];t->age=++age;
            int dx=x*TILE-left,dy=y*TILE-top;
            int x0=dx<0?-dx:0, x1=w-dx<TILE?w-dx:TILE;
            for(int j=dy<0?-dy:0;j<TILE && dy+j<h;j++) {
                unsigned short *out=s->pixels+(BAR+dy+j)*w+dx+x0;
                unsigned char *in=t->image->pixels+j*TILE+x0;
                for(int k=x0;k<x1;k++) *out++=t->image->colors[*in++];
            }
        }
    }
    char title[48];snprintf(title,sizeof title,"Map  Z%d%s",zoom,error?"  Offline / retry":fd>=0?"  Loading":"");
    app_rect(s,0,0,w,BAR,0x1948);app_text(s,6,2,title,0xffff);
    app_rect(s,w-72,0,72,BAR,0x236c);app_text(s,w-59,2,"-",0xffff);app_text(s,w-23,2,"+",0xffff);
    app_rect(s,0,s->height-FOOT,w,FOOT,0x1948);
    app_text(s,4,s->height-FOOT,"(c) OpenStreetMap contributors",0xffff);
}
int main(void) { return app_run(event,draw); }
