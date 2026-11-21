#include "window-app.h"
#include "../console.h"
#include <stdio.h>
#include <string.h>

/* Snapshot 29/9 contains three counters and a packed series of complete rows. */
typedef unsigned long long Counter;
typedef struct {
    Counter pid, parent, cpu_ticks, bytes, state, background;
    char name[32];
} Process;
typedef struct {
    Counter wall, idle, ram;
    Process rows[16];
} Snapshot;
static Snapshot previous, current;
static int count, previous_count, offset, held, history_count;
static int cpu[60], memory[60], per_process[16];
static Counter allocated;
static char legacy[4097];
static const unsigned short bg=0x1926, ink=0xffff, aqua=0x87df, gold=0xfd65;

static int percent(Counter part, Counter total) {
    if (!total) return -1;
    if (part >= total) return 100;
    return (int)(100.0*(double)part/(double)total + 0.5);
}
static void sample(void) {
    long bytes=console_call(29,9);
    previous=current;
    previous_count=history_count?count:0;
    count=0;
    if(bytes<24 || bytes>(long)sizeof current || (bytes-24)%sizeof(Process)) {
        current.wall=0;
        history_count=0;
        long n=console_call(29,6);
        if(n>0 && n<=4096) {
            memcpy(legacy,(void *)0x443d7000,n);
            legacy[n]=0;
            for(char *p=legacy;*p;p++) if(*p=='\n' && p[1]) count++;
        } else legacy[0]=0;
        return;
    }
    memcpy(&current,(void *)0x443d7000,bytes);
    count=(bytes-24)/sizeof(Process);
    Counter elapsed=current.wall-previous.wall;
    allocated=0;
    for(int i=0;i<count;i++) {
        Process *p=&current.rows[i];
        allocated+=p->bytes;
        per_process[i]=-1;
        for(int j=0;j<previous_count;j++) {
            Process *old=&previous.rows[j];
            if(p->pid==old->pid && p->cpu_ticks>=old->cpu_ticks)
                per_process[i]=percent(p->cpu_ticks-old->cpu_ticks,elapsed);
        }
    }
    if(history_count==60) {
        memmove(cpu,cpu+1,59*sizeof(int)); memmove(memory,memory+1,59*sizeof(int));
        history_count--;
    }
    Counter idle=current.idle-previous.idle;
    cpu[history_count]=previous.wall && elapsed ? percent(elapsed>idle?elapsed-idle:0,elapsed):-1;
    memory[history_count++]=percent(allocated,current.ram);
}
static int event(AppWindow *s, WindowEvent e) {
    s->timer_ms=1000;
    int refresh=e.type==WINDOW_CLIENT_TIMER || (e.type==WINDOW_CLIENT_CONFIGURE && !history_count);
    if(refresh) sample();
    int step=0;
    if(e.type==WINDOW_CLIENT_KEY) step=(e.value=='j')-(e.value=='k');
    if(e.type==WINDOW_CLIENT_POINTER) {
        if(e.value && !held && e.y<23 && e.x>=s->width-54)
            step=e.x>=s->width-27?1:-1;
        held=e.value;
    }
    int visible=(s->height-96)/22;
    if(visible<1) visible=1;
    if(offset+step>=0) offset+=step;
    if(offset>count-visible) offset=count-visible;
    if(offset<0) offset=0;
    return refresh || step;
}
static void plot(AppWindow *s,int x,int width,const int *values,unsigned short color) {
    app_rect(s,x,45,width,25,0x2209);
    int last=-1,oldx=0;
    for(int i=0;i<history_count;i++) {
        int px=x+(60-history_count+i)*(width-1)/59;
        if(values[i]<0) {last=-1; continue;}
        int y=68-values[i]*22/100;
        if(last>=0 && px>oldx) {
            int prev=last;
            for(int j=oldx+1;j<=px;j++) {
                int next=last+(y-last)*(j-oldx)/(px-oldx);
                app_rect(s,j,next<prev?next:prev,1,1+(next>prev?next-prev:prev-next),color);prev=next;
            }
        } else app_rect(s,px,y,1,2,color);
        last=y;oldx=px;
    }
}
static void draw(AppWindow *s) {
    char text[128], usage[12];
    app_rect(s,0,0,s->width,s->height,bg);
    snprintf(text,sizeof text,"Tasks  %d",count);
    app_text(s,4,1,text,ink);app_text(s,s->width-52,1," ^  v",aqua);
    if(!history_count) {
        app_text(s,4,24,"CPU/RAM unavailable on this kernel",gold);
        char *p=legacy;
        for(int row=0,y=50;*p && y<s->height;row++) {
            char *end=strchr(p,'\n');
            if(end) *end=0;
            if(!row || row>offset) {app_text(s,4,y,p,ink); y+=22;}
            if(!end) break;
            *end='\n';p=end+1;
        }
        return;
    }
    int half=s->width/2;
    int busy=cpu[history_count-1];
    if(busy<0) strcpy(text,"CPU --"); else snprintf(text,sizeof text,"CPU %d%%",busy);
    app_text(s,4,22,text,aqua);
    snprintf(text,sizeof text,"Proc RAM %llu MiB",allocated>>20);
    app_text(s,half+3,22,text,gold);
    plot(s,4,half-8,cpu,aqua);plot(s,half+3,half-8,memory,gold);
    app_text(s,4,72," PID CPU%  MiB PROCESS",aqua);
    if(s->width>=520) app_text(s,295,72," PPID STATE   MODE",aqua);
    const char *states[]={"?","run","ready","sleep","wait","exit"};
    for(int i=offset,y=94;i<count && y<s->height;i++,y+=22) {
        Process *p=&current.rows[i];
        if(per_process[i]<0) strcpy(usage,"--"); else snprintf(usage,sizeof usage,"%d",per_process[i]);
        snprintf(text,sizeof text,"%4llu %4s %4llu %.16s",p->pid,usage,p->bytes>>20,p->name[0]?p->name:"(system)");
        app_text(s,4,y,text,ink);
        if(s->width>=520) {
            snprintf(text,sizeof text,"%5llu %-6s %s",p->parent,states[p->state<=5?p->state:0],p->background?"bg":"fg");
            app_text(s,295,y,text,ink);
        }
    }
}
int main(void) { return app_run(event,draw); }
