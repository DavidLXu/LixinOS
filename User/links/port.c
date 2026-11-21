/* Links text-mode document engine with the synchronous LixinOS terminal frontend. */
#include "links.h"
#include "load.h"
#include "window.h"
#include "page.h"
#include "unicode.h"
#include "../console.h"
#include "../tcc/lixin.h"

enum { HISTORY = 24 };
typedef struct { char url[ADDRESS_LIMIT]; int top, selected; } Location;
typedef struct {
    struct f_data *doc;
    struct page_media media;
    int cols, rows, depth, armed_link;
    struct browser_window window;
    Location current, back[HISTORY];
    char cwd[513];
    const char *message;
} Browser;

#ifdef LX_DESKTOP_BROWSER
// Remember the source offset because resizing can replace the formatted document.
static int editing_url, form_position = -1;
static char edit_url[ADDRESS_LIMIT];
#endif

static void out(const char *s) {
    while (*s) console_call(1, 256 + (unsigned char)*s++);
}
static void print_char(unsigned ch) {
    if (ch==WIDE_TAIL) return;
    if (ch<32 || (ch>=127 && ch<160) || ch>0x10ffff) ch=' ';
    if (ch<128) {put_byte(ch);return;}
    if (ch>=0x10000) {put_byte(0xf0|(ch>>18));put_byte(0x80|((ch>>12)&63));}
    else if (ch>=0x800) put_byte(0xe0|(ch>>12));
    else {put_byte(0xc0|(ch>>6));put_byte(0x80|(ch&63));return;}
    put_byte(0x80|((ch>>6)&63));put_byte(0x80|(ch&63));
}
static void text(const char *s,int room) {
    while (*s && room>0) {
        unsigned ch=utf8_next(&s);
        room-=han_slot(ch)>=0?2:1;
        if (room>=0) print_char(ch);
    }
}
static int address(Browser *b, const char *s, char *dest) {
    int n;
    if (strstr(s,"://") || !strcmp(s,"about:links"))
        n=snprintf(dest,ADDRESS_LIMIT,"%s",s);
    else if (*s=='/') n=snprintf(dest,ADDRESS_LIMIT,"file://%s",s);
    else n=snprintf(dest,ADDRESS_LIMIT,"file://%s%s%s",b->cwd,
                    !strcmp(b->cwd,"/")?"":"/",s);
    return n>=0 && n<ADDRESS_LIMIT;
}
static const char home[]="<title>Links 2.30</title><h1>Links 2.30</h1>"
    "<p><a href=\"https://example.com/\">Example Domain</a></p><p>Press G to open an HTML file or an HTTP or HTTPS address.</p>"
    "<p>Tab selects a link. Enter follows it. B goes back. Q quits.</p>";
static struct f_data *layout(Browser *b,const char *url,char *headers,char *html,size_t len,const char *title) {
    struct cache_entry *entry=calloc(1,sizeof(*entry)+strlen(url));
    if (!entry) fatal_exit("not enough memory");
    strcpy((char *)entry->url,url);
    entry->head=(unsigned char *)headers;
    struct document_options opts={0};
    opts.xw=b->cols; opts.yw=b->rows; opts.tables=1; opts.break_long_lines=1;
    opts.cp=utf8_table;
    opts.assume_cp=utf8_table;
    opts.framename=(unsigned char *)"";
    color2rgb(&opts.default_fg,0xffffff); color2rgb(&opts.default_link,0x00ffff);
    struct f_data *doc=init_formatted(&opts);
    struct object_request rq={0};
    rq.ce=entry; rq.url=entry->url; doc->rq=&rq;
    really_format_html(entry,(unsigned char *)html,(unsigned char *)html+len,doc,0);
    doc->rq=NULL;
    if (title) {
        mem_free(doc->title);
        doc->title=stracpy((unsigned char *)title);
    }
    free(entry); return doc;
}
static int visit(Browser *b, Location loc, int remember) {
    struct page_source src={0};
    if (!strcmp(loc.url,"about:links")) {
        src.body=(char *)home; src.length=sizeof(home)-1;
    } else {
        const char *err=load_page(loc.url,&src);
        if (err) { b->message=err; return 0; }
    }
    struct page_media media={0};
    const char *error=prepare_page(&src,loc.url,b->window.canvas.pixels?(b->cols-4)*WEB_CHAR:0,&media);
    if (error) { release_page(&media); free(src.bytes); b->message=error; return 0; }
    struct f_data *doc=layout(b,loc.url,src.headers,src.body,src.length,media.title);
    release_page(&b->media); b->media=media;
    free(src.bytes);
    if (b->doc && remember) {
        if (b->depth==HISTORY) {
            memmove(b->back,b->back+1,(HISTORY-1)*sizeof(Location));
            --b->depth;
        }
        b->back[b->depth++]=b->current;
    }
    if (b->doc) destroy_formatted(b->doc);
    b->armed_link=-1; b->doc=doc; b->current=loc; b->message=b->media.script_errors?(b->media.first_error[0]?b->media.first_error:"Some page scripts could not run"):NULL;
    if (b->current.selected>=doc->nlinks) b->current.selected=-1;
    if (loc.selected<0) {
        for (int i=0;i<doc->nlinks;i++) if ((doc->links[i].where || doc->links[i].form) && doc->links[i].n) {
            b->current.selected=i; break;
        }
        const char *fragment=strchr(loc.url,'#');
        if (fragment && fragment[1]) {
            struct tag *tag; struct list_head *it;
            foreach(struct tag,tag,it,doc->tags) if (!strcmp((char *)tag->name,fragment+1)) {
                b->current.top=tag->y; break;
            }
        }
    }
    int last=doc->y>b->rows ? doc->y-b->rows : 0;
    if (b->current.top>last) b->current.top=last;
    return 1;
}
static int tick(Browser *b) {
    int changed=page_tick(&b->media);
    if (b->media.navigate[0]) {
        Location next={.selected=-1}; strcpy(next.url,b->media.navigate);
        b->media.navigate[0]=0;
        visit(b,next,!b->media.replace); return 1;
    }
    if (!changed) return 0;
    if (!page_snapshot(&b->media)) {b->message="Page update exceeds available memory";return 0;}
    struct f_data *doc=layout(b,b->current.url,b->media.headers,b->media.html,strlen(b->media.html),b->media.title);
    destroy_formatted(b->doc); b->doc=doc;
    if (b->current.selected>=doc->nlinks) b->current.selected=-1;
    int last=doc->y>b->rows?doc->y-b->rows:0;
    if (b->current.top>last) b->current.top=last;
    return 1;
}
static int active_link(struct link *l) {
    return l->where || (l->form && l->form->ro!=2);
}
static void draw_window(Browser *b,const char *address) {
    struct browser_window *w=&b->window;
    const char *status=b->message;
    if (!status && b->current.selected>=0) status=(char *)b->doc->links[b->current.selected].where;
    window_frame(w,(char *)b->doc->title,address,status ? status : "Tab: select    Enter: open    G: address    Q: close");
    for (int row=0;row<b->rows && row+b->current.top<b->doc->y;row++) {
        int y=row+b->current.top;
        unsigned char links[256]={0};
        for (int i=0;i<b->doc->nlinks;i++) {
            struct link *link=&b->doc->links[i];
            if (!active_link(link)) continue;
            for (int j=0;j<link->n;j++) {
                struct point pos=link->pos[j];
                if (pos.y==y && (unsigned)pos.x<(unsigned)b->cols) links[pos.x]=i==b->current.selected ? 2 : 1;
            }
        }
        for (int x=0;x<b->cols && x<b->doc->data[y].l;x++) {
            unsigned ch; unsigned char attr;
            get_char_attr(b->doc,x,y,&ch,&attr);
            if (attr & ATTR_FRAME) ch=ch==196?'-':ch==179?'|':'+';
            if (!draw_picture(w,&b->media,ch,x,row)) window_char(w,x,row,ch,links[x]);
        }
    }
    window_present(w);
}
static void draw(Browser *b) {
    if (b->window.canvas.pixels) { draw_window(b,b->current.url); return; }
    const char *title=(char *)b->doc->title;
    out("\033[H\033[7mLinks 2.30 | ");
    text(*title?title:b->current.url,b->cols-14);
    out("\033[0m\033[K");
    struct link *link=b->current.selected>=0 ? &b->doc->links[b->current.selected] : NULL;
    for (int i=0;i<b->rows;i++) {
        out("\r\n\033[K");
        int y=b->current.top+i;
        if (y>=b->doc->y) continue;
        unsigned char marked[256]={0};
        if (link) for (int j=0;j<link->n;j++) {
            struct point pos=link->pos[j];
            if (pos.y==y && (unsigned)pos.x<(unsigned)b->cols) marked[pos.x]=1;
        }
        int on=0;
        for (int x=0;x<b->doc->data[y].l && x<b->cols;x++) {
            unsigned ch; unsigned char attr;
            get_char_attr(b->doc,x,y,&ch,&attr);
            if (marked[x]!=on) {on=marked[x]; out(on?"\033[7m":"\033[0m");}
            if (attr & ATTR_FRAME) ch=ch==196?'-':ch==179?'|':'+';
            print_char(ch);
        }
        if (on) out("\033[0m");
    }
    out("\r\n\033[K");
    text(b->message?b->message:link && link->where?(char *)link->where:b->current.url,b->cols);
    out("\r\n\033[K");
    text("Tab: link  Enter: open  B: back  Space: page  G: URL  Q: quit",b->cols);
}
static void select_link(Browser *b,int direction) {
    int n=b->doc->nlinks, index=b->current.selected;
    for (int i=0;i<n;i++) {
        index=(index+direction+n)%n;
        struct link *link=&b->doc->links[index];
        if (!active_link(link) || !link->n) continue;
        b->current.selected=index;
        int y=link->pos[0].y;
        if (y<b->current.top) b->current.top=y;
        if (y>=b->current.top+b->rows) b->current.top=y-b->rows+1;
        return;
    }
}
static int read_input(Browser *b,char *url,const char *label) {
    size_t n=0;
    if (!b->window.canvas.pixels) {out("\r\033[K");text(label,b->cols-2);out(": ");}
    for (;;) {
        if (b->window.canvas.pixels) {url[n]=0;draw_window(b,url);}
        int k=lx_key_byte();
        if (k==27 || k==3 || k>=256 || k<0) return 0;
        if (k==13 || k==10) {url[n]=0; return n!=0;}
        if (k==8 || k==127) { if (n) {--n;if (!b->window.canvas.pixels) out("\b \b");} }
        else if (k>=32 && k<127 && n+1<ADDRESS_LIMIT) {url[n++]=k;if (!b->window.canvas.pixels) put_byte(k);}
    }
}
static int query_part(char *url,size_t *used,const unsigned char *s) {
    const char hex[]="0123456789ABCDEF";
    for (;s && *s;s++) {
        unsigned c=*s;
        if (*used+3>=ADDRESS_LIMIT) return 0;
        if ((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || strchr("-._~",c))
            url[(*used)++]=c;
        else if (c==' ') url[(*used)++]='+';
        else {url[(*used)++]='%';url[(*used)++]=hex[c>>4];url[(*used)++]=hex[c&15];}
    }
    url[*used]=0; return 1;
}
static void submit_form(Browser *b,struct form_control *trigger) {
    if (trigger->method!=FM_GET || !trigger->action) {b->message="Only GET forms are supported";return;}
    Location next={.selected=-1};
    const char *action=(char *)trigger->action;
    if (strncmp(action,"https://",8) && strncmp(action,"http://",7)) {b->message="Unsupported form action";return;}
    size_t n=strcspn(action,"?#");
    if (n>=sizeof(next.url)-2) goto too_long;
    memcpy(next.url,action,n);next.url[n]=0;
    char separator='?';
    struct form_control *fc;struct list_head *it;
    foreach(struct form_control,fc,it,b->doc->forms) {
        if (fc->form_num!=trigger->form_num || fc->ro==2 || !fc->name || !*fc->name) continue;
        switch (fc->type) {
        case FC_SUBMIT: if (fc!=trigger) continue;break;
        case FC_HIDDEN: case FC_TEXT: case FC_TEXTAREA: break;
        default: continue;
        }
        if (n+2>=sizeof(next.url)) goto too_long;
        next.url[n++]=separator;separator='&';
        if (!query_part(next.url,&n,fc->name) || n+2>=sizeof(next.url)) goto too_long;
        next.url[n++]='=';
        if (!query_part(next.url,&n,fc->default_value)) goto too_long;
    }
    visit(b,next,1);return;
too_long:
    b->message="Form URL too long";
}
static void activate_control(Browser *b,struct form_control *fc) {
    int prevented=page_click(&b->media,fc->position);
    if (!prevented && (fc->type==FC_TEXT || fc->type==FC_TEXTAREA)) {
#ifdef LX_DESKTOP_BROWSER
        if (b->window.hosted) {
            form_position = fc->position;
            editing_url = 1;
            edit_url[0] = 0;
            return;
        }
#endif
        char value[ADDRESS_LIMIT];
        if (read_input(b,value,fc->name?(char *)fc->name:"Value")) {
            mem_free(fc->default_value);
            fc->default_value=stracpy((unsigned char *)value);
            submit_form(b,fc);
        }
    } else if (!prevented && fc->type==FC_SUBMIT) submit_form(b,fc);
    else tick(b);
}
enum { PREV=256, NEXT, PAGE_UP, PAGE_DOWN };
static int pointer_key(Browser *b, struct lx_pointer p) {
    struct browser_window *w=&b->window;
    int x=(int)p.x-w->x, y=(int)p.y-w->y;
    int pressed=p.down && !w->down && p.inside && !p.cancelled;
    int released=!p.down && w->down && p.inside && !p.cancelled;
    int armed=b->armed_link;
    if (pressed || !p.down || p.cancelled || !p.inside) b->armed_link=-1;
    w->mouse_x=p.x; w->mouse_y=p.y; w->inside=p.inside; w->down=p.down;
    if (!p.down || p.cancelled) w->dragging=0;
    if (w->dragging) {
        int nx=(int)p.x-w->dx, ny=(int)p.y-w->dy;
        w->x=nx<0?0:nx+w->w>(int)w->canvas.width?(int)w->canvas.width-w->w:nx;
        w->y=ny<0?0:ny+w->h>(int)w->canvas.height?(int)w->canvas.height-w->h:ny;
    }
    if (x<0 || y<0 || x>=w->w || y>=w->h) return 0;
    if (p.wheel) return p.wheel>0?PAGE_UP:PAGE_DOWN;
    if (!pressed && !released) return 0;
    if (y<44) {
        if (!pressed) return 0;
        if (x>=w->w-35 && x<w->w-7 && y>=8 && y<36) return 'q';
        w->dragging=1; w->dx=x; w->dy=y; return 0;
    }
    if (y<78) return pressed?(x<78?'b':'g'):0;
    if (x<WEB_MARGIN || y<WEB_TOP || y>=w->h-30) return 0;
    int col=(x-WEB_MARGIN)/WEB_CHAR, row=b->current.top+(y-WEB_TOP)/WEB_LINE;
    for (int i=0;i<b->doc->nlinks;i++) {
        struct link *link=&b->doc->links[i];
        if (!active_link(link)) continue;
        for (int j=0;j<link->n;j++) if (link->pos[j].x==col && link->pos[j].y==row) {
            if (pressed) { b->current.selected=i; b->armed_link=i; }
            return released && armed==i ? 13 : 0;
        }
    }
    return 0;
}
static int key(Browser *b) {
    int k;
    if (b->window.canvas.pixels) {
        for (;;) {
            struct lx_pointer p;
            if (lx_gui_pointer(&p)) return pointer_key(b,p);
            k=lx_timed_byte(16);
            if (tick(b)) { if (k!=256) break; return 0; }
            if (k!=256) break;
        }
    } else {
        for (;;) {k=lx_timed_byte(24); if (tick(b) && k==256) return 0; if (k!=256) break;}
    }
    if (k!=27) return k>=256?'q':k;
    if (lx_timed_byte(80)!='[') return 0;
    switch(lx_timed_byte(80)) {
        case 'A': case 'Z': return PREV;
        case 'B': return NEXT;
        case 'C': return 13;
        case 'D': return 'b';
        case '5': return lx_timed_byte(80)=='~'?PAGE_UP:0;
        case '6': return lx_timed_byte(80)=='~'?PAGE_DOWN:0;
        default: return 0;
    }
}
static int browser_action(Browser *b,int k) {
    b->message=NULL;
    if (k=='q'||k=='Q'||k==3||k==4) return 0;
    if (k=='\t'||k==NEXT) select_link(b,1);
    else if (k==PREV) select_link(b,-1);
    else if (k==' '||k==PAGE_DOWN) {
        int last=b->doc->y-b->rows;
        if (last>b->current.top) {
            b->current.top+=b->rows;
            if (b->current.top>last) b->current.top=last;
        }
    } else if (k==PAGE_UP) {
        b->current.top=b->current.top>b->rows?b->current.top-b->rows:0;
    } else if (k=='b'||k=='B'||k==127||k==8) {
        if (b->depth && visit(b,b->back[b->depth-1],0)) --b->depth;
    } else if (k=='g'||k=='G') {
        char input[ADDRESS_LIMIT];
        if (read_input(b,input,"Open")) {
            Location next={.selected=-1};
            if (address(b,input,next.url)) visit(b,next,1);
            else b->message="Address too long";
        }
    } else if ((k==13||k==10) && b->current.selected>=0) {
        struct link *link=&b->doc->links[b->current.selected];
        if (link->form) { activate_control(b,link->form); return 1; }
        char *url=(char *)link->where;
        if (!url) return 1;
        Location next={.selected=-1};
        if ((!strncmp(b->current.url,"http://",7) || !strncmp(b->current.url,"https://",8)) && !strncmp(url,"file:",5))
            b->message="Remote documents cannot open guest files";
        else if (strlen(url)>=sizeof(next.url)) b->message="Address too long";
        else {strcpy(next.url,url); visit(b,next,1);}
    }
    return 1;
}
static int browse(Browser *b,const char *arg) {
    Location loc={.selected=-1};
    if (!address(b,arg,loc.url)) {lx_error("links: address too long\n");return 1;}
    if (!visit(b,loc,0)) {lx_error("links: ");lx_error(b->message);lx_error("\n");return 1;}
    if (!b->window.canvas.pixels) out("\033[2J");
    for (;;) {
        draw(b);
        int k=key(b);
        if (!k) continue;
        if (!browser_action(b,k)) break;
    }
    release_page(&b->media); destroy_formatted(b->doc);
    if (!b->window.canvas.pixels) out("\033[0m\033[2J\033[H");
    return 0;
}
#ifdef LX_DESKTOP_BROWSER
#include "../desktop/browser.h"
static Browser *session;


static void geometry(Browser *b, int x, int y, int width, int height) {
    struct browser_window *w=&b->window;
    w->x=x; w->y=y; w->w=width; w->h=height;
    b->cols=(width-2*WEB_MARGIN)/WEB_CHAR;
    if (b->cols>256) b->cols=256;
    b->rows=(height-WEB_TOP-30)/WEB_LINE;
}

int desk_web_open(unsigned *pixels,int stride,int height,const char *cwd,int width,int rows) {
    if (session) return 1;
    Browser *b=calloc(1,sizeof(*b));
    if (!b) return 0;
    b->window.canvas=(struct lx_canvas){.pixels=pixels,.width=stride,.height=height};
    b->window.hosted=1;
    strcpy(b->cwd,cwd);
    geometry(b,0,0,width,rows);
    static int initialized;
    if (!initialized) { init_charset(); initialized=1; }
    Location home_loc={.selected=-1};
    strcpy(home_loc.url,"about:links");
    if (!visit(b,home_loc,0)) { free(b); return 0; }
    session=b;
    return 1;
}
void desk_web_close(void) {
    if (!session) return;
    release_page(&session->media);
    destroy_formatted(session->doc);
    free(session); session=NULL; editing_url=0; form_position=-1;
}
const char *desk_web_title(void) { return session ? (char *)session->doc->title : "Links"; }
void desk_web_draw(int x,int y,int w,int h) {
    Browser *b=session;
    int resized=b->window.w!=w || b->window.h!=h;
    geometry(b,x,y,w,h);
    if (resized && b->media.html) {
        struct f_data *doc=layout(b,b->current.url,b->media.headers,b->media.html,strlen(b->media.html),b->media.title);
        destroy_formatted(b->doc); b->doc=doc;
        if (b->current.selected>=doc->nlinks) b->current.selected=-1;
        int last=doc->y>b->rows?doc->y-b->rows:0;
        if (b->current.top>last) b->current.top=last;
    }
    draw_window(b,editing_url?edit_url:b->current.url);
}
int desk_web_tick(void) { return session && !editing_url && tick(session); }
int desk_web_key(int key) {
    Browser *b=session;
    if (editing_url) {
        size_t n=strlen(edit_url);
        if (key==27 || key==3) { editing_url=0; form_position=-1; }
        else if (key==13 || key==10) {
            Location loc={.selected=-1};
            editing_url=0;
            if (form_position >= 0 && n) {
                for (int i=0;i<b->doc->nlinks;i++) {
                    struct form_control *fc=b->doc->links[i].form;
                    if (fc && fc->position==form_position) {
                        mem_free(fc->default_value);
                        fc->default_value=stracpy((unsigned char *)edit_url);
                        submit_form(b,fc);
                        break;
                    }
                }
            } else if (n && address(b,edit_url,loc.url)) visit(b,loc,1);
            form_position=-1;
        } else if ((key==8 || key==127) && n) edit_url[n-1]=0;
        else if (key>=32 && key<127 && n+1<sizeof(edit_url)) { edit_url[n]=key; edit_url[n+1]=0; }
    } else if (key=='g' || key=='G') { editing_url=1; edit_url[0]=0; }
    else return browser_action(b,key);
    return 1;
}
void desk_web_cancel_pointer(void) {
    if (!session) return;
    session->armed_link=-1;
    session->window.down=session->window.dragging=0;
}
int desk_web_pointer(int x,int y,int down,int wheel) {
    if (editing_url) return 0;
    struct lx_pointer p={.x=x,.y=y,.down=down,.wheel=wheel,.inside=1};
    int k=pointer_key(session,p);
    if (k) desk_web_key(k);
    return k || down;
}
#else
extern char __data_image[], __data_begin[], __data_end[];
/* The browser heap is mapped separately. Use the private RW bank below
   the mailboxes for the C/JS stack; preserve the kernel-supplied arguments. */
__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        "ldr x9, =__user_heap_limit\nmov sp, x9\nb browser_start\n.previous");
__attribute__((noreturn))
void browser_start(const char *cwd,const char *args,size_t len) {
    memcpy(__data_begin,__data_image,__data_end-__data_begin);
    if (len==10 && !memcmp(args,"--version",10)) {out("Links 2.30 (LixinOS)\r\n");finish(0);}
    int graphics=len>=3 && !memcmp(args,"-g",3);
    if (graphics) { args+=3; len-=3; }
    if (len && (len>ADDRESS_LIMIT || strnlen(args,len)!=len-1)) {
        lx_error("usage: links [-g] [file | URL]\n");finish(2);
    }
    Browser *browser=calloc(1,sizeof(*browser));
    if (!browser || strlen(cwd)>=sizeof(browser->cwd)) finish(1);
    strcpy(browser->cwd,cwd);
    char initial[ADDRESS_LIMIT];
    strcpy(initial,len?args:"about:links");
    if (!lx_stdout_is_terminal()) {lx_error("links requires an interactive terminal\n");finish(2);}
    unsigned long screen=lx_geometry();
    browser->cols=screen & 65535;
    if (browser->cols<16 || browser->cols>256) browser->cols=80;
    browser->rows=((screen>>16)&65535)-3;
    if (browser->rows<1 || browser->rows>100) browser->rows=21;
    if (graphics) {
        if (!window_open(&browser->window)) {lx_error("links -g requires a foreground QEMU window\n");finish(1);}
        browser->cols=(browser->window.w-2*WEB_MARGIN)/WEB_CHAR;
        if (browser->cols>256) browser->cols=256;
        browser->rows=(browser->window.h-WEB_TOP-30)/WEB_LINE;
    }
    init_charset();
    finish(browse(browser,initial));
}

#endif
