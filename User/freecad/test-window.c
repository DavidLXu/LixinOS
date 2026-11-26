#include <lixin.h>
#include "/src/console.h"
typedef unsigned long size_t;
#define NULL ((void*)0)
#include <string.h>
#include <stdio.h>
#include "/src/gui/text-clipboard.h"
#include "/src/desktop/window-client-host.h"

static WindowClient client;
static int send(int type, int x, int y, int value) {
    client_event(&client, (WindowEvent){type,x,y,value});
    long end = console_call(38,0) + 30000;
    while (client.count || client.busy) {
        if (client_tick(&client,0) < 0 || console_call(38,0) >= end) return 0;
        if (client.busy) console_call(43,1);
    }
    return 1;
}

// Counts only the model wireframe and hashes its positions inside the viewport.
static unsigned geometry(void) {
    unsigned hash=0;
    for (int y=30; y<client.height-25; ++y)
        for (int x=0; x<client.width; ++x)
            if (client.pixels[y*client.width+x]==0x966f) hash=hash*31+y*client.width+x;
    return hash;
}
int main(void) {
    if (!client_open_streams(&client,"/","/bin/freecad-part.win3",0,2)) return 1;
    if (!send(WINDOW_CLIENT_CONFIGURE,520,340,0)) return 2;
    unsigned initial=geometry();
    if (!initial || !send(WINDOW_CLIENT_KEY,0,0,'r') || geometry()==initial) return 3;
    unsigned rotated=geometry();
    if (!send(WINDOW_CLIENT_POINTER,180,12,1) || !send(WINDOW_CLIENT_POINTER,180,12,0)
        || geometry()==rotated) return 4;
    if (!send(WINDOW_CLIENT_KEY,0,0,'s')) return 5;
    if (!send(WINDOW_CLIENT_CONFIGURE,420,260,0) || client.width!=420 || !geometry()) return 6;
    client_event(&client,(WindowEvent){WINDOW_CLIENT_CLOSE,0,0,0});
    long end=console_call(38,0)+10000;
    while (client_tick(&client,0)>=0) {
        if (console_call(38,0)>=end) return 7;
        console_call(43,1);
    }
    puts("FreeCAD Win3 render, rotate, edit, save and resize passed");
    return 0;
}
