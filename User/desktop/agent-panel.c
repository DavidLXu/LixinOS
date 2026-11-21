/* The native control window. The project harness owns execution and releases. */
#include "/src/desktop/window-app.h"
#include "/src/console.h"

typedef unsigned long size_t;
unsigned long strlen(const char *);
int strcmp(const char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
int snprintf(char *, unsigned long, const char *, ...);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);

#ifndef NULL
#define NULL ((void *)0)
#endif
#include "/src/gui/text-clipboard.h"
#include "/src/desktop/window-client-host.h"

#define APP "/bin/agent"
#define PYTHON "/bin/python3"
#define WORKBENCH "/src/agent/desktop.py"

static struct {
    char project[513], selected[64], input[1025], queued[1025];
    char status[361], notice[96], candidate[65], current[65], previous_id[65], approved[65];
    struct { char name[64], label[128], state[16]; } sessions[12];
    struct { char state[20], text[361]; } events[40];
    char packet[19000];
    int count, event_count, offset, input_len, edit_project, review;
    int ready, previous, confirm, active, info, fd, used, held, refresh, stopping;
    int preview, preview_focus;
    WindowClient previews[2];
} ui;

static void copy(char *out, const char *in, unsigned size) {
    unsigned i = 0;
    while (i+1 < size && in[i]) { out[i] = in[i]; i++; }
    out[i] = 0;
}

#include "/src/desktop/agent-session-scan.h"

static int spawn(const char *cwd, const char *file, const char *const *args, int out) {
    char *mail = (char *)0x443d8000;
    unsigned size = 0;
    for (int i = -2; i < 0 || args[i]; i++) {
        const char *s = i == -2 ? cwd : i == -1 ? file : args[i];
        unsigned n = strlen(s) + 1;
        if (size+n > 4096) return -1;
        memcpy(mail+size,s,n);
        size += n;
    }
    return console_call(50, ((unsigned long)size << 32) | (255L << 16) | ((long)out << 8) | 255L);
}

static void query(void) {
    if (ui.info || ui.edit_project || !ui.project[0]) return;
    long pipe = console_call(49, 0);
    if (pipe < 0) return;
    char names[AGENT_SESSION_LIMIT][AGENT_TRACE_NAME_LIMIT];
    int count = scan_sessions(ui.project,names);
    const char *args[7 + AGENT_SESSION_LIMIT] = {PYTHON, "-I", WORKBENCH, "panel-info", ui.selected,
                         ui.review ? "review" : "timeline"};
    for (int i=0;i<count;i++) args[6+i]=names[i];
    int pid = spawn(ui.project, PYTHON, args, pipe & 255);
    lx_close(pipe & 255);
    if (pid < 1) { lx_close(pipe >> 32); return; }
    ui.info = pid;
    ui.fd = pipe >> 32;
    ui.used = 0;
    ui.refresh = 0;
}

static char *field(char **line) {
    char *text = *line, *next = strchr(text, '\t');
    if (next) { *next = 0; *line = next + 1; }
    else *line += strlen(text);
    return text;
}

static void unpack(void) {
    if (!ui.review) ui.count = 0;
    ui.event_count = ui.ready = ui.previous = 0;
    ui.status[0] = 0;
    char *line = ui.packet;
    while (*line) {
        char *next = strchr(line,'\n');
        if (!next) break;
        *next++ = 0;
        char *kind = field(&line);
        if (!strcmp(kind,"P")) {
            ui.ready = *field(&line) == '1';
            ui.previous = *field(&line) == '1';
            copy(ui.candidate,field(&line),sizeof ui.candidate);
            copy(ui.current,field(&line),sizeof ui.current);
            copy(ui.previous_id,field(&line),sizeof ui.previous_id);
            copy(ui.status,line,sizeof ui.status);
        } else if (!strcmp(kind,"S") && ui.count < 12) {
            int i = ui.count++;
            copy(ui.sessions[i].name,field(&line),sizeof ui.sessions[i].name);
            copy(ui.sessions[i].state,field(&line),sizeof ui.sessions[i].state);
            copy(ui.sessions[i].label,line,sizeof ui.sessions[i].label);
        } else if (!strcmp(kind,"E") && ui.event_count < 40) {
            int i = ui.event_count++;
            copy(ui.events[i].state,field(&line),sizeof ui.events[i].state);
            copy(ui.events[i].text,line,sizeof ui.events[i].text);
        }
        line = next;
    }
}

static int collect(void) {
    if (!ui.info) return 0;
    char data[4096];
    while (lx_poll(ui.fd,LX_READABLE,0) > 0) {
        int n = lx_read(ui.fd,data,sizeof data);
        if (n <= 0) break;
        if (ui.used+n < (int)sizeof ui.packet) {
            memcpy(ui.packet+ui.used,data,n);
            ui.used += n;
        }
    }
    long status = console_call(51, (1UL<<32) | (unsigned)ui.info);
    if (status == -2) return 0;
    lx_close(ui.fd);
    ui.info = 0;
    ui.packet[ui.used] = 0;
    if (status == 0) unpack();
    else {
        ui.count = ui.event_count = ui.ready = ui.previous = 0;
        copy(ui.status,"Open a valid desktop project",sizeof ui.status);
    }
    return 1;
}

static int preview_width(AppWindow *s) { return (s->width-36)/2; }
static int preview_height(AppWindow *s) { return s->height-244; }

static void close_preview(void) {
    if (!ui.preview) return;
    for (int i=0;i<2;i++) client_close(&ui.previews[i]);
    ui.preview=0;
    ui.preview_focus=-1;
}

static void configure_preview(AppWindow *s) {
    for (int i=0;i<2;i++)
        if (ui.previews[i].pid && !ui.previews[i].stopping)
            client_event(&ui.previews[i],(WindowEvent){WINDOW_CLIENT_CONFIGURE,preview_width(s),preview_height(s),0});
}

static void open_preview(AppWindow *s) {
    if (ui.preview) { close_preview(); return; }
    if (ui.active || !ui.ready || s->width<390 || preview_height(s)<64 ||
        ui.previews[0].pid || ui.previews[1].pid) return;
    const char *slash = strrchr(ui.project,'/');
    if (!slash || !slash[1]) return;
    char path[513];
    if (ui.current[0]) {
        snprintf(path,sizeof path,"/bin/%s.win2",slash+1);
        client_open(&ui.previews[0],ui.project,path);
    }
    int length=snprintf(path,sizeof path,"%s/.desktop/versions/%s/%s.win2",ui.project,ui.candidate,slash+1);
    if (length<0 || length>=(int)sizeof path || !client_open(&ui.previews[1],ui.project,path)) {
        client_close(&ui.previews[0]);
        copy(ui.notice,"Could not open the candidate preview",sizeof ui.notice);
        return;
    }
    ui.preview=1;
    ui.preview_focus=-1;
    configure_preview(s);
}

static void draw_previews(AppWindow *s) {
    int w=preview_width(s),h=preview_height(s);
    for (int i=0;i<2;i++) {
        int x=12+i*(w+12), y=145;
        app_text(s,x,120,i?"Candidate":"Installed",app_rgb(118,225,201));
        WindowClient *c=&ui.previews[i];
        app_rect(s,x,y,w,h,app_rgb(23,34,48));
        if (c->ready && c->width>0 && c->height>0) {
            for (int j=0;j<h;j++) for(int k=0;k<w;k++)
                s->pixels[(y+j)*s->width+x+k]=c->pixels[(j*c->height/h)*c->width+k*c->width/w];
        } else if (w>=180) app_text(s,x+8,y+8,c->pid?"Starting preview":"No app to preview",app_rgb(147,167,186));
    }
}

static void work(const char *op, const char *task) {
    if (ui.active || ui.edit_project) return;
    close_preview();
    const char *args[] = {APP,"desktop-work",ui.project,op,task,ui.selected,0};
    if (!strcmp(op,"resume")) { args[4] = ui.selected; args[5] = 0; }
    if (!strcmp(op,"publish") || !strcmp(op,"rollback")) { args[4] = ui.approved; args[5] = 0; }
    int pid = spawn(ui.project, APP, args, 255);
    if (pid > 0) {
        ui.active = pid;
        ui.refresh = 1;
        if (task || !strcmp(op,"resume")) {
            ui.selected[0] = 0;
            ui.review = ui.offset = 0;
        }
        copy(ui.notice,"Agent is working",sizeof ui.notice);
    } else copy(ui.notice,"Could not start the Agent",sizeof ui.notice);
}

static void send(void) {
    if (ui.edit_project) {
        if (ui.input[0] != '/' || strlen(ui.input) >= sizeof ui.project || ui.info) return;
        copy(ui.project,ui.input,sizeof ui.project);
        ui.edit_project = ui.count = ui.event_count = ui.review = ui.offset = ui.confirm = 0;
        ui.selected[0] = ui.status[0] = ui.notice[0] = 0;
        ui.refresh = 1;
    } else if (ui.input_len) {
        if (ui.active) {
            copy(ui.queued,ui.input,sizeof ui.queued);
            copy(ui.notice,"Follow-up queued",sizeof ui.notice);
        } else work("send",ui.input);
    }
    ui.input_len = 0;
    ui.input[0] = 0;
}

static void edit_root(void) {
    if (ui.active || ui.info) return;
    close_preview();
    ui.confirm = 0;
    ui.edit_project = 1;
    copy(ui.input,ui.project,sizeof ui.input);
    ui.input_len = strlen(ui.input);
}

static unsigned short color(unsigned r, unsigned g, unsigned b) { return app_rgb(r,g,b); }
static int sidebar(AppWindow *s) { return !ui.preview && s->width >= 550 ? 226 : 0; }
static void label(AppWindow *s, int x, int y, int space, const char *text, unsigned short ink) {
    char visible[170];
    int cols = space / 11;
    if (cols < 1) return;
    if (cols >= (int)sizeof visible) cols = sizeof visible-1;
    copy(visible,text,cols+1);
    app_text(s,x,y,visible,ink);
}

static void button(AppWindow *s,int x,int y,int w,const char *text,int enabled) {
    app_rect(s,x,y,w,28,enabled ? color(39,77,82) : color(34,46,62));
    label(s,x+6,y+3,w-10,text,enabled ? color(201,242,233) : color(123,145,156));
}

static void draw(AppWindow *s) {
    unsigned short bg=color(16,25,37), panel=color(23,34,48), ink=color(225,234,241);
    unsigned short teal=color(118,225,201), muted=color(147,167,186), line=color(46,63,81);
    int width=s->width, height=s->height, side=sidebar(s);
    app_rect(s,0,0,width,height,bg);
    app_rect(s,0,0,width,76,panel);
    label(s,14,8,width-30,"LixinOS Agent",teal);
    label(s,14,42,width-130,ui.project,ink);
    if (width>220) button(s,width-106,39,92,"Project",!ui.active);
    if (width>=390 && ui.ready) button(s,width-215,39,102,ui.preview?"Back":"Preview",!ui.active);
    app_rect(s,0,76,width,1,line);
    if (side) {
        app_rect(s,0,77,side,height-143,panel);
        label(s,14,84,side-24,"Sessions",teal);
        for (int i=0;i<ui.count && 110+i*35+30<height-70;i++) {
            int y=111+i*35;
            int chosen=ui.selected[0] ? !strcmp(ui.selected,ui.sessions[i].name) : i==0;
            if (chosen) app_rect(s,7,y,side-14,31,color(41,67,78));
            label(s,13,y+4,side-26,ui.sessions[i].label,chosen?ink:muted);
        }
    }
    int x=side+12, room=width-x-10;
    label(s,x,85,room,ui.status[0]?ui.status:"Choose a desktop project",ui.ready?teal:muted);
    int first=ui.review?ui.offset:ui.event_count-(height-220)/34-ui.offset;
    if (first<0) first=0;
    if (ui.preview) draw_previews(s);
    for (int i=first,y=115;!ui.preview && i<ui.event_count && y+36<height-97;i++,y+=34) {
        app_rect(s,x,y,room,32,panel);
        label(s,x+5,y+5,90,ui.events[i].state,teal);
        label(s,x+93,y+5,room-97,ui.events[i].text,ink);
    }
    if (width>=460) {
        int y=height-95;
        button(s,x,y,100,ui.active?"Stop":"Continue",ui.active||ui.count);
        button(s,x+108,y,92,ui.review?"Timeline":"Review",1);
        button(s,x+208,y,104,ui.confirm==1?"Confirm":"Publish",ui.ready&&!ui.active);
        if (width-x>418) button(s,x+320,y,100,ui.confirm==2?"Confirm":"Rollback",ui.previous&&!ui.active);
    }
    app_rect(s,0,height-63,width,63,panel);
    int input_room=width-108;
    app_rect(s,10,height-57,input_room,30,bg);
    const char *entry=ui.input;
    int cap=(input_room-12)/11;
    if (cap>0 && ui.input_len>cap) entry+=ui.input_len-cap;
    label(s,16,height-53,input_room-12,ui.input_len?entry:(ui.edit_project?"Project path":"Message the Agent"),ui.input_len?ink:muted);
    if (width>118) button(s,width-89,height-56,80,ui.edit_project?"Open":"Send",1);
    label(s,12,height-25,width-24,ui.notice[0]?ui.notice:"Enter to send  |  Up / Down to scroll",muted);
}

static int event(AppWindow *s, WindowEvent e) {
    if (e.type == WINDOW_CLIENT_CONFIGURE) {
        if (!ui.project[0]) { copy(ui.project,"/work",sizeof ui.project); edit_root(); }
        if (ui.preview) {
            if (s->width<390 || preview_height(s)<64) close_preview();
            else configure_preview(s);
        }
        s->timer_ms=ui.preview ? 40 : 250;
        return 1;
    }
    if (e.type == WINDOW_CLIENT_TIMER) {
        int changed=collect();
        for (int i=0;i<2;i++) {
            int result=client_tick(&ui.previews[i],ui.preview);
            if (result<0) client_close(&ui.previews[i]);
            if (result) changed=1;
        }
        s->timer_ms=ui.preview ? 40 : 250;
        if (ui.active) {
            long status=console_call(51,(1UL<<32)|(unsigned)ui.active);
            if (status!=-2) {
                ui.active=0;
                ui.confirm=0;
                copy(ui.notice,ui.stopping?"Stopped":status==0?"Task finished":"Task ended with an error",sizeof ui.notice);
                ui.stopping=0;
                ui.refresh=changed=1;
                if (ui.queued[0]) { work("send",ui.queued); ui.queued[0]=0; }
            }
        }
        if (ui.active||ui.refresh) query();
        return changed;
    }
    if (e.type == WINDOW_CLIENT_PASTE) {
        for (int i=0;i<e.value && ui.input_len<(int)sizeof ui.input-1;i++)
            if (s->paste[i]>=32 && s->paste[i]!=127) ui.input[ui.input_len++]=s->paste[i];
        ui.input[ui.input_len]=0;
        return 1;
    }
    if (e.type == WINDOW_CLIENT_KEY) {
        int k=e.value;
        if (ui.preview && ui.preview_focus>=0) {
            if (k==27) { close_preview(); return 1; }
            client_event(&ui.previews[ui.preview_focus],e);
            return 0;
        }
        if (k==13||k==10) send();
        else if (k==27) { ui.confirm=0; if (ui.edit_project && ui.status[0]) { ui.edit_project=0; ui.input_len=0; ui.input[0]=0; } }
        else if (k==302 && ui.offset<ui.event_count-1) ui.offset++;
        else if (k==303 && ui.offset>0) ui.offset--;
        else if ((k==127||k==8) && ui.input_len) ui.input[--ui.input_len]=0;
        else if (k>=32 && k<256 && ui.input_len<(int)sizeof ui.input-1) {
            ui.input[ui.input_len++]=k; ui.input[ui.input_len]=0;
        }
        return 1;
    }
    if (e.type == WINDOW_CLIENT_POINTER) {
        int edge=e.value&&!ui.held,release=!e.value&&ui.held; ui.held=e.value;
        if (ui.preview) {
            int w=preview_width(s),h=preview_height(s);
            for(int i=0;i<2;i++) {
                int px=12+i*(w+12);
                if (edge && app_hit(e,px,145,w,h)) ui.preview_focus=i;
                if ((edge||release) && ui.preview_focus==i && (release||app_hit(e,px,145,w,h))) {
                    client_event(&ui.previews[i],(WindowEvent){WINDOW_CLIENT_POINTER,e.x-px,e.y-145,e.value});
                    return 0;
                }
            }
        }
        if (!edge) return 0;
        int x=sidebar(s)+12,h=s->height;
        if (app_hit(e,s->width-106,39,92,28)) edit_root();
        else if (s->width>=390 && ui.ready && app_hit(e,s->width-215,39,102,28)) {
            open_preview(s); s->timer_ms=ui.preview?40:250;
        }
        else if (app_hit(e,s->width-89,h-56,80,28)) send();
        else if (sidebar(s) && e.x<sidebar(s) && e.y>=111 && e.y<h-70) {
            int i=(e.y-111)/35;
            if (i<ui.count) {
                copy(ui.selected,ui.sessions[i].name,sizeof ui.selected);
                ui.offset=0; ui.review=0; ui.refresh=1;
            }
        } else if (s->width>=460 && e.y>=h-95 && e.y<h-67) {
            if (app_hit(e,x,h-95,100,28)) {
                if (ui.active) { console_call(55,ui.active); ui.stopping=1; ui.queued[0]=0; }
                else if (ui.count) work("resume",0);
            } else if (app_hit(e,x+108,h-95,92,28)) {
                close_preview();
                ui.review=!ui.review; ui.offset=0; ui.refresh=1; ui.confirm=0;
            } else if (app_hit(e,x+208,h-95,104,28) && ui.ready&&!ui.active) {
                if (ui.confirm==1) work("publish",0);
                else { ui.confirm=1; copy(ui.approved,ui.candidate,sizeof ui.approved); }
            } else if (app_hit(e,x+320,h-95,100,28) && ui.previous&&!ui.active) {
                if (ui.confirm==2) work("rollback",0);
                else { ui.confirm=2; copy(ui.approved,ui.previous_id,sizeof ui.approved); }
            }
        }
        return 1;
    }
    return 0;
}

int main(void) {
    int result=app_run3(event,draw);
    for (int i=0;i<2;i++) client_shutdown(&ui.previews[i]);
    if (ui.active) console_call(55,ui.active);
    if (ui.info) { lx_close(ui.fd); console_call(55,ui.info); }
    return result;
}
