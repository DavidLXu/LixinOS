#include "window-widgets.h"
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
enum { SORT_PID, SORT_CPU, SORT_MEMORY };
static int sort_key=SORT_CPU, descending=1, pressed_column=-1;
static void sort_by(int column) {
    descending=sort_key==column ? !descending : column!=SORT_PID;
    sort_key=column;
    offset=0;
}
static int column_at(int x,int y) {
    if (y<72 || y>=94 || x<0 || x>=177) return -1;
    return x<52 ? SORT_PID : x<107 ? SORT_CPU : SORT_MEMORY;
}
static int before(int a,int b) {
    Process *left=&current.rows[a], *right=&current.rows[b];
    int cmp=0;
    if (sort_key==SORT_CPU) {
        if (per_process[a]<0 || per_process[b]<0) {
            if (per_process[a]!=per_process[b]) return per_process[b]<0;
        } else cmp=per_process[a]-per_process[b];
    } else if (sort_key==SORT_MEMORY) cmp=(left->bytes>right->bytes)-(left->bytes<right->bytes);
    else cmp=(left->pid>right->pid)-(left->pid<right->pid);
    if (!cmp) return left->pid<right->pid;
    return descending ? cmp>0 : cmp<0;
}
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
    if(e.type==WINDOW_CLIENT_KEY) {
        step=(e.value=='j')-(e.value=='k');
        if (history_count && (e.value=='c' || e.value=='m' || e.value=='p')) {
            sort_by(e.value=='c'?SORT_CPU:e.value=='m'?SORT_MEMORY:SORT_PID);
            return 1;
        }
    }
    if(e.type==WINDOW_CLIENT_POINTER) {
        if(e.value && !held && e.y<23 && e.x>=s->width-54)
            step=e.x>=s->width-27?1:-1;
        int col=column_at(e.x,e.y);
        if (e.value && !held) pressed_column=col;
        if (!e.value && held && history_count && col>=0 && col==pressed_column) {
            sort_by(col);
            refresh=1;
        }
        held=e.value!=0;
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
    snprintf(text,sizeof text,"进程  %d",count);
    label(s,4,1,s->width-64,text,ink);
    for (int j=0;j<4;j++) {
        app_rect(s,s->width-41-j,7+j,1+2*j,1,aqua);
        app_rect(s,s->width-15-j,10-j,1+2*j,1,aqua);
    }
    if(!history_count) {
        label(s,4,24,s->width-8,"此内核仅提供进程列表",gold);
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
    snprintf(text,sizeof text,"进程内存 %llu MiB",allocated>>20);
    label(s,half+3,22,half-6,text,gold);
    plot(s,4,half-8,cpu,aqua);plot(s,half+3,half-8,memory,gold);
    const int x[]={4,54,108,179};
    const char *titles[]={"PID","CPU","内存","进程名称"};
    app_rect(s,0,72,s->width,22,0x2209);
    for (int c=0;c<4;c++) {
        int width=c<3 ? x[c+1]-x[c]-3 : s->width-x[c];
        label(s,x[c],72,width,titles[c],c==sort_key?gold:ink);
        if (c==sort_key) {
            int tip=x[c]+(c==2?37:35);
            for (int j=0;j<4;j++) app_rect(s,tip+j,78+(descending?j:3-j),7-2*j,1,gold);
        }
    }
    int wide=s->width>=590;
    if(wide) label(s,362,72,s->width-366,"父进程    状态    模式",ink);
    int order[16];
    for (int i=0;i<count;i++) {
        int j=i;
        while (j>0 && before(i,order[j-1])) {order[j]=order[j-1]; j--;}
        order[j]=i;
    }
    const char *states[]={"未知","运行","就绪","休眠","等待","退出"};
    for(int i=offset,y=94;i<count && y+20<=s->height;i++,y+=22) {
        int idx=order[i];
        Process *p=&current.rows[idx];
        if (i%2) app_rect(s,0,y,s->width,22,0x1967);
        snprintf(text,sizeof text,"%llu",p->pid);
        label(s,x[0],y,47,text,ink);
        if(per_process[idx]<0) strcpy(usage,"--"); else snprintf(usage,sizeof usage,"%d%%",per_process[idx]);
        label(s,x[1],y,53,usage,aqua);
        snprintf(text,sizeof text,"%llu MiB",p->bytes>>20);
        label(s,x[2],y,69,text,gold);
        label(s,x[3],y,(wide?356:s->width)-x[3],p->name[0]?p->name:"系统",ink);
        if(wide) {
            snprintf(text,sizeof text,"%llu",p->parent);
            label(s,362,y,72,text,ink);
            label(s,438,y,63,states[p->state<=5?p->state:0],ink);
            label(s,510,y,s->width-511,p->background?"后台":"前台",ink);
        }
    }
}
int main(void) { return app_run(event,draw); }
