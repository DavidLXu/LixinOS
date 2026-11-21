#ifndef LX_FILE_DIALOG_H
#define LX_FILE_DIALOG_H
#include "widgets.h"
#include "directory-nav.h"
#include "han-font.c"

/* Foreground canvas open/save picker. Name entry accepts an absolute path
 * or a name relative to the displayed directory. The caller owns file I/O. */
enum { FD_NAME=1, FD_ACCEPT, FD_CANCEL, FD_UP, FD_TRACK, FD_CRUMB=16, FD_PLACE=540, FD_ROW=600 };
typedef struct {
    char dir[513], name[513], result[513], confirm[513];
    UiPlaces places;
    UiText edit;
    UiControls controls;
    int save,active,first,count,selected,grab;
    const char *hint;
} FileDialog;
static inline int fd_visible(struct lx_canvas *c) { return (c->height-164)/26; }
static inline void fd_scan(FileDialog *d) {
    char name[513];
    d->count=0; d->first=0; d->selected=-1;
    long size;
    while ((size=lx_dir(d->dir,d->count,name))==LX_DIR || size>=0) ++d->count;
    d->hint=size==LX_END ? "" : "Cannot list directory";
}
static inline void fd_begin(FileDialog *d,const char *current,int save) {
    memset(d,0,sizeof *d);
    d->save=save; d->active=1; d->controls.focus=FD_NAME;
    char *mail=(char *)0x443d7000UL;
    long n=lx_console_call(42,0);
    char base[513]="/";
    if (n>0 && n<=512) { memcpy(base,mail,n); base[n]=0; }
    if (!ui_file_path(d->dir,base,current)) strcpy(d->dir,base);
    char *slash=strrchr(d->dir,'/');
    if (save) strcpy(d->name,slash+1);
    slash[slash==d->dir]=0;
    ui_places_read(&d->places);
    fd_scan(d); ui_text_begin(&d->edit,d->name,1);
}
static inline struct lx_button fd_button(struct lx_canvas *c,FileDialog *d,int id) {
    if (id==FD_UP) return (struct lx_button){20,14,68,34,"Up"};
    if (id==FD_ACCEPT) return (struct lx_button){c->width-212,c->height-48,90,34,d->save?"Save":"Open"};
    return (struct lx_button){c->width-112,c->height-48,90,34,"Cancel"};
}
static inline int fd_width(const char *s,int from,int end) {
    const char *p=s+from;
    int w=0;
    while (p<s+end) w+=han_slot(utf8_next(&p))>=0 ? 18 : 9;
    return w;
}
static inline void fd_label(struct lx_canvas *c,struct lx_font *f,int x,int y,int room,const char *s,unsigned color) {
    while (*s) {
        unsigned cp=utf8_next(&s); int wide=han_slot(cp)>=0, w=wide ? 18 : 9;
        if (w>room) break;
        if (wide) {
            const unsigned char *bits=lx_han_glyph(cp);
            if (bits) for (int j=0;j<16;++j) for (int i=0;i<16;++i)
                if (bits[2*j+i/8] & (128>>(i%8))) lx_gui_rect(c,x+i,y+3+j,1,1,color);
        } else {
            char ch[2]={cp=='\t'?' ':cp<127?(char)cp:'?',0};
            lx_gui_text(c,f,x,y,1,ch,color);
        }
        x+=w; room-=w;
    }
}
static inline int fd_offset(FileDialog *d,int room) {
    int start=0, w=fd_width(d->name,0,d->edit.caret);
    while (w>room && start<d->edit.caret) {
        int next=ui_next(d->name,start);
        w-=fd_width(d->name,start,next); start=next;
    }
    return start;
}
static inline int fd_measure(const char *name) { return fd_width(name,0,strlen(name)); }
static inline int fd_crumbs(FileDialog *d,struct lx_canvas *c,struct lx_font *f,int hit) {
    UiCrumb crumbs[UI_CRUMBS];
    int n=ui_crumbs(d->dir,c->width-122,fd_measure,crumbs);
    for (int i=0;i<n;++i) {
        UiCrumb b=crumbs[i]; int x=102+b.x;
        if (f) {
            char name[513]; ui_crumb_label(d->dir,b,name);
            lx_gui_rect(c,x,14,b.width-2,34,0xe0e9e2);
            fd_label(c,f,x+4,20,b.width-8,name,0x233b43);
        } else if (hit>=x && hit<x+b.width) return FD_CRUMB+b.end;
    }
    return 0;
}
static inline int fd_sidebar(FileDialog *d,struct lx_canvas *c) {
    return d->places.count ? ui_limit(c->width/3,150) : 0;
}
static inline int fd_visit(FileDialog *d,const char *path,int keep_name) {
    if (lx_stat(path)!=LX_DIR) { d->hint="Folder unavailable."; return 0; }
    strcpy(d->dir,path);
    if (!d->save || !keep_name || strchr(d->name,'/')) d->name[0]=0;
    d->confirm[0]=0; fd_scan(d);
    ui_text_begin(&d->edit,d->name,1); d->controls.focus=FD_NAME;
    return 0;
}
static inline void fd_draw(FileDialog *d,struct lx_canvas *c,struct lx_font *f) {
    lx_gui_rect(c,0,0,c->width,c->height,0xf3f5ef);
    fd_crumbs(d,c,f,0);
    for (int id=FD_ACCEPT;id<=FD_UP;++id) {
        struct lx_button b=fd_button(c,d,id);
        lx_gui_button(c,f,&b,d->controls.pressed==id?0x163c3a:id==FD_ACCEPT?0x24736f:0x405b65,0xffffff);
        if (d->controls.focus==id) lx_gui_rect(c,b.x+4,b.y+b.h-4,b.w-8,2,0xb8d6cf);
    }
    int rows=fd_visible(c), side=fd_sidebar(d,c);
    if (side) {
        lx_gui_rect(c,18,60,side-3,rows*26,0xe5ece5);
        fd_label(c,f,25,61,side-14,"收藏夹",0x526c6d);
        for (int i=0;i<rows-1 && i+d->places.first<d->places.count;++i) {
            const char *path=d->places.path[i+d->places.first];
            int y=86+i*26;
            if (!strcmp(path,d->dir) || d->controls.focus==FD_PLACE+i+d->places.first) lx_gui_rect(c,19,y,side-5,25,0xc4e1d7);
            fd_label(c,f,25,y+1,side-14,ui_place_label(path),0x233b43);
        }
    }
    for (int i=0;i<rows && i+d->first<d->count;++i) {
        char name[513]; long size=lx_dir(d->dir,i+d->first,name);
        if (size<0 && size!=LX_DIR) break;
        int y=60+i*26;
        lx_gui_rect(c,18+side,y,c->width-44-side,25,d->selected==d->first+i?0xc4e1d7:0xffffff);
        fd_label(c,f,26+side,y+1,18,size==LX_DIR?"/":"",0x24736f);
        fd_label(c,f,48+side,y+1,c->width-86-side,name,0x233b43);
    }
    if (d->count>rows) {
        UiThumb thumb=ui_list_thumb(d->first,d->count,rows,rows*26);
        lx_gui_rect(c,c->width-24,60,6,rows*26,0xd5dfda);
        lx_gui_rect(c,c->width-24,60+thumb.start,6,thumb.length,0x526c6d);
    }
    int y=c->height-92, room=c->width-58, start=fd_offset(d,room);
    lx_gui_rect(c,20,y,c->width-40,31,0xffffff);
    if (d->controls.focus==FD_NAME) {
        int lo=d->edit.anchor<d->edit.caret?d->edit.anchor:d->edit.caret,hi=d->edit.caret+d->edit.anchor-lo;
        int left=fd_width(d->name,start,lo),right=ui_limit(fd_width(d->name,start,hi),room);
        if (right>left) lx_gui_rect(c,27+left,y+3,right-left,24,0xc4e1d7);
        lx_gui_rect(c,27+fd_width(d->name,start,d->edit.caret),y+4,1,22,0x24736f);
    }
    fd_label(c,f,27,y+4,room,d->name+start,0x233b43);
    fd_label(c,f,20,c->height-39,c->width-238,d->hint,0x405b65);
}
static inline int fd_action(FileDialog *d,int id) {
    if (!id) return 0;
    if (id==FD_CANCEL) { d->active=0; return 0; }
    if (id>=FD_CRUMB && id<FD_PLACE) {
        char path[513]; ui_crumb_target(d->dir,id-FD_CRUMB,path);
        return fd_visit(d,path,1);
    }
    if (id>=FD_PLACE && id<FD_ROW) return fd_visit(d,d->places.path[id-FD_PLACE],1);
    if (id>=FD_ROW) {
        d->selected=id-FD_ROW;
        char item[513];
        long size=lx_dir(d->dir,d->selected,item);
        if (size<0 && size!=LX_DIR) { fd_scan(d); return 0; }
        strcpy(d->name,item);
        ui_text_begin(&d->edit,d->name,1);
        d->controls.focus=FD_NAME; d->confirm[0]=0;
        return 0;
    }
    if (id!=FD_ACCEPT && id!=FD_UP) return 0;
    if (!ui_file_path(d->result,d->dir,id==FD_UP?"..":d->name)) {
        d->hint="Enter a path of up to 512 bytes."; return 0;
    }
    long size=lx_stat(d->result);
    if (size==LX_DIR) return fd_visit(d,d->result,id==FD_UP);
    if (size<0 && !(d->save && size==LX_MISSING)) { d->hint="File unavailable."; return 0; }
    if (d->save && size>=0 && strcmp(d->confirm,d->result)) {
        strcpy(d->confirm,d->result); d->hint="File exists. Choose Save again to replace it."; return 0;
    }
    return 1;
}
static inline int fd_key(FileDialog *d,struct lx_canvas *c,int key) {
    if (key==27) return fd_action(d,FD_CANCEL);
    if (key==9) {
        int ids[4+UI_FAVORITES]={FD_NAME,FD_ACCEPT,FD_CANCEL,FD_UP};
        for (int i=0;i<d->places.count;++i) ids[4+i]=FD_PLACE+i;
        int focus=ui_focus_next(d->controls.focus,ids,4+d->places.count);
        if (focus>=FD_PLACE) d->places.first=ui_list_reveal(d->places.first,focus-FD_PLACE,
            d->places.count,fd_visible(c)-1);
        d->controls.focus=focus; return 0;
    }
    if (key==302 || key==303) {
        if (d->count) {
            int row=ui_limit(d->selected+(key==302?-1:1),d->count-1);
            d->first=ui_list_reveal(d->first,row,d->count,fd_visible(c));
            fd_action(d,FD_ROW+row);
        }
    } else if (key==13 || key==10 || (key==' ' && d->controls.focus!=FD_NAME))
        return fd_action(d,d->controls.focus==FD_NAME?FD_ACCEPT:d->controls.focus);
    else if (d->controls.focus==FD_NAME && ui_text_key(&d->edit,d->name,sizeof d->name,key)) {
        d->confirm[0]=0; d->hint=""; d->selected=-1;
    }
    return 0;
}
static inline int fd_pointer(FileDialog *d,struct lx_canvas *c,struct lx_pointer *p) {
    int rows=fd_visible(c),x=p->x,y=p->y,hit=0;
    if (p->cancelled) { d->controls.pressed=d->controls.down=0; return 0; }
    if (p->inside) {
        int side=fd_sidebar(d,c);
        if (p->wheel) {
            if (x>=18 && x<18+side && y>=60 && y<60+26*rows)
                d->places.first=ui_list_top(d->places.first-p->wheel,d->places.count,rows-1);
            else d->first=ui_list_top(d->first-p->wheel,d->count,rows);
            return 0;
        }
        if (y>=14 && y<48) hit=fd_crumbs(d,c,NULL,x);
        if (x>=20 && x<(int)c->width-20 && y>=(int)c->height-92 && y<(int)c->height-61) hit=FD_NAME;
        for (int id=FD_ACCEPT;id<=FD_UP;++id) {
            struct lx_button b=fd_button(c,d,id);
            if (lx_gui_hit(&b,p)) hit=id;
        }
        if (y>=60 && y<60+26*rows && x>=18 && x<(int)c->width-16) {
            int row=d->first+(y-60)/26;
            if (x<18+side) {
                int i=d->places.first+(y-60)/26-1;
                if (y>=86 && i<d->places.count) hit=FD_PLACE+i;
            } else if (x>=(int)c->width-26 && d->count>rows) hit=FD_TRACK;
            else if (row<d->count) hit=FD_ROW+row;
        }
    }
    int press=p->down && !d->controls.down;
    int action=ui_pointer(&d->controls,hit,p->down);
    if (d->controls.pressed==FD_TRACK) {
        UiThumb t=ui_list_thumb(d->first,d->count,rows,rows*26);
        if (press) d->grab=y>=60+t.start && y<60+t.start+t.length ? y-60-t.start : t.length/2;
        d->first=ui_list_drag(y-60-d->grab,t.travel,d->count,rows);
    }
    if (d->controls.pressed==FD_NAME) {
        int i=fd_offset(d,c->width-58),w=x-27;
        while (d->name[i]) {
            int next=ui_next(d->name,i),step=fd_width(d->name,i,next);
            if (w<step/2) break;
            w-=step; i=next;
        }
        d->edit.caret=i; if (press) d->edit.anchor=i;
    }
    return fd_action(d,action);
}
#endif
