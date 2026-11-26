/* Guest integration fixture. Drive the same pipes and decoder as Desktop. */
#include <lixin.h>
#include "/src/console.h"
typedef unsigned long size_t;
#ifndef NULL
#define NULL ((void*)0)
#endif
#include <string.h>
#include <stdio.h>
#include "/src/gui/text-clipboard.h"
#include "/src/desktop/window-client-host.h"

static WindowClient viewer;

static int event(int kind,int x,int y,int value) {
    client_event(&viewer,(WindowEvent){kind,x,y,value});
    long deadline=console_call(38,0)+90000;
    while (viewer.count || viewer.busy) {
        int status=client_tick(&viewer,0);
        if (status<0 || console_call(38,0)>=deadline) {
            printf("Win3 event %d at %d,%d value %d failed: %s\n",kind,x,y,value,
                   viewer.error ? viewer.error : status<0 ? "child exited" : "deadline expired");
            return 0;
        }
        if (viewer.busy && lx_poll(viewer.output,LX_READABLE,0)<=0) console_call(43,1);
    }
    return 1;
}

static int wait_model(const char *name) {
    for (int i=0; i<40; ++i) {
        if (!strcmp(viewer.title,name)) return 1;
        if (!event(WINDOW_CLIENT_TIMER,0,0,0)) return 0;
    }
    printf("Expected %s, got %s\n",name,viewer.title);
    return 0;
}

static int open_model(const char *name) {
    unsigned length=strlen(name);
    if (!event(WINDOW_CLIENT_KEY,2,0,15)) return 0;
    memcpy(viewer.paste,name,length);
    return event(WINDOW_CLIENT_PASTE,0,0,length) && event(WINDOW_CLIENT_KEY,0,0,13);
}

static unsigned viewport_hash(void) {
    unsigned hash=2166136261U;
    for (int y=100;y<viewer.height-20;y+=3)
        for (int x=viewer.width/2-45;x<viewer.width/2+45;x+=3)
            hash=(hash^viewer.pixels[y*viewer.width+x])*16777619U;
    return hash;
}

static int read_time(char time[32]) {
    char report[4097];
    if (!event(WINDOW_CLIENT_KEY,2,0,4)) return 0;
    int fd=lx_open("/MJDATA.TXT");
    if (fd<0) return 0;
    int n=lx_read(fd,report,sizeof report-1);
    lx_close(fd);
    if (n<1) return 0;
    report[n]=0;
    for (int i=0;i+7<n;i++) {
        if (strncmp(report+i,"TIME = ",7)) continue;
        int start=i+7, length=0;
        while (start+length<n && report[start+length]!='\n' && length<31) ++length;
        memcpy(time,report+start,length);
        time[length]=0;
        return length>0;
    }
    return 0;
}

static int arrow(int final) {
    return event(WINDOW_CLIENT_KEY,0,0,27) && event(WINDOW_CLIENT_KEY,0,0,'[') &&
           event(WINDOW_CLIENT_KEY,0,0,final);
}

static int click(int x,int y) {
    return event(WINDOW_CLIENT_POINTER,x,y,1) && event(WINDOW_CLIENT_POINTER,x,y,0);
}

int main(void) {
    if (!client_open_streams(&viewer,"/","/bin/simulate.win3",0,2)) return 1;
    if (!event(WINDOW_CLIENT_CONFIGURE,721,437,15) || !wait_model("MuJoCo : Humanoid")) return 2;
    if (!event(WINDOW_CLIENT_KEY,0,0,' ') || !event(WINDOW_CLIENT_KEY,2,0,16)) return 3;
    if (lx_stat("/screenshot.png")<1000) return 4;
    unsigned paused=viewport_hash();
    if (!event(WINDOW_CLIENT_TIMER,0,0,0) || viewport_hash()!=paused) return 5;
    if (!event(WINDOW_CLIENT_POINTER,356,238,16) || !event(WINDOW_CLIENT_POINTER,392,248,16) ||
        !event(WINDOW_CLIENT_POINTER,392,248,0) || viewport_hash()==paused) return 6;
    unsigned panned=viewport_hash();
    if (!event(WINDOW_CLIENT_WHEEL,371,212,1) || viewport_hash()==panned) return 7;
    puts("Testing MJCF and MJB export");
    if (!click(57,50) || !click(180,50) || lx_stat("/mjmodel.xml")<100 || lx_stat("/mjmodel.mjb")<100) return 8;
    if (!open_model("/lib/simulate/triple.mjb") || !wait_model("MuJoCo : lixinos-three-link")) return 9;
    char initial[32], stepped[32], rewind[32];
    if (!read_time(initial) || !arrow('C') || !read_time(stepped) || !strcmp(initial,stepped) ||
        !arrow('D') || !read_time(rewind) || strcmp(initial,rewind)) return 10;
    if (!client_drop_file(&viewer,"/lib/simulate/slider_crank.xml") ||
        !wait_model("MuJoCo : slider-crank")) return 11;
    if (!event(WINDOW_CLIENT_CONFIGURE,803,451,15) || viewer.width!=803 || viewer.height!=451) return 12;
    client_event(&viewer,(WindowEvent){WINDOW_CLIENT_CLOSE,0,0,0});
    long deadline=console_call(38,0)+15000;
    while (client_tick(&viewer,0)>=0) {
        if (console_call(38,0)>=deadline) return 13;
        console_call(43,1);
    }
    puts("MuJoCo viewer passed");
    return 0;
}
