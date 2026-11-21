#include "window-app.h"
#include "../console.h"
#include <stdio.h>

static int year, month, today_year, today_month, today, held;
static long last_day = -1;
static const char *names[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static int days(int y, int m) {
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[m-1] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0));
}
static int event(AppWindow *s, WindowEvent e) {
    long stamp = console_call(36,0)/86400;
    int changed = stamp != last_day;
    if (changed) {
        last_day = stamp;
        today_year = 1970;
        while (stamp >= 337 + days(today_year,2)) stamp -= 337 + days(today_year++,2);
        today_month = 1;
        while (stamp >= days(today_year,today_month)) stamp -= days(today_year,today_month++);
        today = stamp + 1;
    }
    s->timer_ms = 30000;
    int key = e.type == WINDOW_CLIENT_KEY ? e.value : 0;
    if (e.type == WINDOW_CLIENT_POINTER) {
        if (e.value && !held && e.y < 25)
            key = e.x < 32 ? '[' : e.x >= s->width-32 ? ']' : 't';
        held = e.value;
    }
    if (!year || key == 't') { year=today_year; month=today_month; return 1; }
    if (key == '[' && (year>1 || month>1)) {
        if (--month == 0) { month=12; --year; }
        return 1;
    }
    if (key == ']' && (year<9999 || month<12)) {
        if (++month == 13) { month=1; ++year; }
        return 1;
    }
    return changed;
}
static void draw(AppWindow *s) {
    char text[32];
    unsigned short ink=0x1926, accent=0x246c;
    app_rect(s,0,0,s->width,s->height,0xffde);
    snprintf(text,sizeof text,"%s %04d",names[month-1],year);
    app_text(s,7,1,"<",ink); app_text(s,(s->width-72)/2,1,text,ink);
    app_text(s,s->width-19,1,">",ink);
    const char *week[]={"Mo","Tu","We","Th","Fr","Sa","Su"};
    int cell=s->width/7, row=(s->height-46)/6;
    if(row<1) row=1;
    for(int i=0;i<7;i++) app_text(s,i*cell+(cell-18)/2,24,week[i],accent);
    int y=year-1, first=(365*y+y/4-y/100+y/400)%7;
    for(int m=1;m<month;m++) first=(first+days(year,m))%7;
    for(int d=1;d<=days(year,month);d++) {
        int i=first+d-1,x=i%7*cell,top=46+i/7*row;
        int highlight=year==today_year && month==today_month && d==today;
        if(highlight) app_rect(s,x,top,cell,row,accent);
        snprintf(text,sizeof text,"%2d",d);
        app_text(s,x+(cell-18)/2,top,text,highlight?0xffff:ink);
    }
}
int main(void) { return app_run(event,draw); }
