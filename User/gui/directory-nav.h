#ifndef LX_DIRECTORY_NAV_H
#define LX_DIRECTORY_NAV_H
#include "file-path.h"

/* A component's end offset identifies its exact absolute path, independent of
 * clipping. Keep the root and as many trailing components as will fit. */
enum { UI_CRUMBS=24, UI_FAVORITES=24 };
typedef struct { int x,width,start,end; } UiCrumb;
static inline int ui_crumbs(const char *path,int room,int (*measure)(const char *),UiCrumb out[UI_CRUMBS]) {
    if (room<22) return 0;
    out[0]=(UiCrumb){0,22,0,1};
    int end=strlen(path), n=1, remaining=room-22;
    while (end>1 && n<UI_CRUMBS && remaining>=22) {
        int start=end-1;
        while (path[start-1]!='/') --start;
        char name[513]; memcpy(name,path+start,end-start); name[end-start]=0;
        int width=measure(name)+14;
        if (width>remaining) {
            if (n>1) break;
            width=remaining;
        }
        out[n++]=(UiCrumb){0,width,start,end};
        remaining-=width; end=start-1;
    }
    int x=22;
    for (int i=n-1;i>0;--i) { out[i].x=x; x+=out[i].width; }
    return n;
}
static inline void ui_crumb_label(const char *path,UiCrumb crumb,char out[513]) {
    int n=crumb.end-crumb.start;
    memcpy(out,path+crumb.start,n); out[n]=0;
}
static inline void ui_crumb_target(const char *path,int end,char out[513]) {
    memcpy(out,path,end); out[end]=0;
}

#define UI_PLACES_DIR "/home/lixin/.file-bookmarks"
typedef struct { int count,first,slot[UI_FAVORITES]; char path[UI_FAVORITES][513]; } UiPlaces;
static inline void ui_place_record(int slot,char record[64]) {
    strcpy(record,UI_PLACES_DIR "/A");
    record[sizeof(UI_PLACES_DIR)]='A'+slot;
}
static inline void ui_places_read(UiPlaces *p) {
    p->count=0;
    for (int slot=0;slot<UI_FAVORITES;++slot) {
        char record[64],canonical[513];
        ui_place_record(slot,record);
        long size=lx_stat(record);
        if (size<1 || size>512) continue;
        int fd=lx_open(record), used=0;
        if (fd<0) continue;
        char *path=p->path[p->count];
        while (used<size) {
            int got=lx_read(fd,path+used,size-used);
            if (got<=0) break;
            used+=got;
        }
        lx_close(fd); path[used]=0;
        if (used==size && (int)strlen(path)==used && path[0]=='/' &&
            ui_file_path(canonical,"/",path) && !strcmp(path,canonical)) p->slot[p->count++]=slot;
    }
}
static inline int ui_place_index(const UiPlaces *p,const char *path) {
    for (int i=0;i<p->count;++i) if (!strcmp(p->path[i],path)) return i;
    return -1;
}
static inline const char *ui_place_label(const char *path) {
    return path[1] ? strrchr(path,'/')+1 : "/";
}
#endif
