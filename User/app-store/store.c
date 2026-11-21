#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../desktop/window-widgets.h"
#include "../console.h"

enum Page { HOME, FORM, LIBRARY, PROJECT, PACKAGE };
enum Action { DISCOVER=1, LIB, REFRESH, VIBE, PORT, BRIEF, SOURCE, CREATE, SEND,
              CHECK, PUBLISH, ROLLBACK, REMOVE, PREV, NEXT, IDEA=30, ITEM=40 };
enum { BG=0xf7bb, INK=0x11e8, TEAL=0x1b8d, DIM=0x52ea, PALE=0xe739, WHITE=0xffff };
static const char worker[]="/lib/app-store/store.py";
static enum Page page;
static UiText editor, location;
static UiControls ctl;
static char ideas[3][260],suggestions[3][260],brief[1025],source[513],key[80],title[1025];
static char current[66],candidate[66],previous[66],notice[400],origin[64];
static char package_bins[300],pkg_name[80];
static struct { char kind[12],key[80],name[320]; } items[5];
static int scroll,content_h,port,idea_count,lib_page,total,item_count,confirm;
static unsigned seed, hint_seed;
typedef struct { int pid,fd,used,overlong; char line[2048]; } Job;
static Job hints,work;
static int made,error_seen;
static AppControl controls[24];
static int ncontrols;

static void copy(char *out,size_t size,const char *s) { snprintf(out,size,"%s",s); }
static void set_page(enum Page next) {
    page=next; scroll=confirm=0;
    ctl.focus=next==FORM || next==PROJECT ? BRIEF : DISCOVER;
}
static void fresh_ideas(void) {
    static const struct { const char *subject, *action[3]; } prompts[] = {
        {"读书摘记",{"按作者归档","标出想重读的段落","随机抽取一条摘记"}},
        {"习惯日历",{"点日期打卡","用颜色标出连续完成的天数","按周回顾"}},
        {"像素画板",{"用鼠标涂色","支持镜像绘图","能撤销最后一笔"}},
        {"旅行清单",{"拖动行李排序","按地点分组","点选已装包的物品"}},
        {"单词卡",{"点击翻面","收集总记错的词","随机出一道拼写题"}},
        {"节拍练习器",{"跟着闪灯敲空格","点按两次设定速度","切换三拍和四拍"}},
        {"番茄钟",{"用方向键调整时长","空格暂停或继续","画出当天的专注时段"}},
        {"滑块拼图",{"用方向键挪动空格","记录最少步数","一键重玩同一局"}},
        {"观星日志",{"按月份翻阅","点击记录月相","标记想再看的星星"}},
        {"菜谱卡",{"按食材查找","点选做完的步骤","随机挑一道晚餐"}},
        {"配色本",{"点击保存当前颜色","调换两种颜色的位置","显示相邻色对比"}},
        {"零钱账本",{"按类别涂色","累加本周开支","撤销最后一笔记录"}}
    };
    unsigned v=0;
    if (lx_random(&v,sizeof v)<0) v=(unsigned)time(NULL) ^ (unsigned)lx_pid();
    seed=v ^ (seed*1664525u+1013904223u);
    unsigned count=sizeof prompts/sizeof prompts[0];
    for (int i=0;i<3;i++) {
        unsigned which=(seed%count+5*i)%count;
        unsigned variant=(seed/(13+2*i)+i)%3;
        snprintf(ideas[i],sizeof ideas[i],"做一个%s，%s",prompts[which].subject,prompts[which].action[variant]);
    }
    copy(origin,sizeof origin,"本机生成");
}

static int start(Job *job,const char *const argv[]) {
    if (job->pid) return 0;
    long ends=console_call(49,0);
    if (ends<0) return 0;
    char *mail=(char *)0x443d8000;
    unsigned used=0;
    for (int i=-2; i<0 || argv[i]; i++) {
        const char *part=i==-2 ? "/" : i==-1 ? argv[0] : argv[i];
        unsigned n=strlen(part)+1;
        if (used+n>4096) break;
        memcpy(mail+used,part,n); used+=n;
        if (i>=0 && !argv[i+1]) {
            int writefd=ends&255;
            long pid=console_call(50,((unsigned long)used<<32)|((long)writefd<<16)|(writefd<<8)|255);
            lx_close(writefd);
            if (pid>0) { *job=(Job){.pid=pid,.fd=ends>>32}; return 1; }
            lx_close(ends>>32); return 0;
        }
    }
    lx_close(ends&255); lx_close(ends>>32); return 0;
}
static void generate(void) {
    fresh_ideas();
    if (!hints.pid) {
        char nonce[16]; snprintf(nonce,sizeof nonce,"%u",seed);
        const char *args[]={"/bin/agent","desktop-work","/","store-ideas",nonce,NULL};
        idea_count=0; hint_seed=seed;
        start(&hints,args);
    }
}
static void command(const char *op,const char *a,const char *b,const char *c) {
    const char *args[]={"/bin/python3","-I",worker,op,a,b,c,NULL};
    made=error_seen=0;
    copy(notice,sizeof notice,!strcmp(op,"send") ? "Agent 正在修改代码和测试…" : "正在读取或更新软件…");
    if (!start(&work,args)) copy(notice,sizeof notice,"无法启动商店任务");
}
static void library(int index) {
    char number[20]; snprintf(number,sizeof number,"%d",index);
    set_page(LIBRARY); item_count=0;
    command("library",number,NULL,NULL);
}
static char *field(char **s) {
    char *first=*s,*tab=strchr(first,'\t');
    if (tab) { *tab=0; *s=tab+1; } else *s+=strlen(*s);
    return first;
}
static void line(Job *job,char *s) {
    char *type=field(&s);
    if (job==&hints) {
        if (!strcmp(type,"IDEA") && idea_count<3) copy(suggestions[idea_count++],sizeof suggestions[0],s);
        return;
    }
    if (!strcmp(type,"ERROR")) { copy(notice,sizeof notice,s); error_seen=1; }
    else if (!strcmp(type,"PROJECT")) { copy(key,sizeof key,s); made=1; }
    else if (!strcmp(type,"PAGE")) { lib_page=atoi(field(&s)); total=atoi(s); }
    else if (!strcmp(type,"ITEM") && item_count<5) {
        int i=item_count++;
        copy(items[i].kind,sizeof items[i].kind,field(&s));
        copy(items[i].key,sizeof items[i].key,field(&s));
        copy(items[i].name,sizeof items[i].name,s);
    } else if (!strcmp(type,"DETAIL")) {
        copy(key,sizeof key,field(&s)); copy(title,sizeof title,field(&s));
        copy(current,sizeof current,field(&s)); copy(candidate,sizeof candidate,field(&s)); copy(previous,sizeof previous,s);
        set_page(PROJECT);
    } else if (!strcmp(type,"BIN") && strlen(package_bins)+strlen(s)+3<sizeof package_bins) {
        if (*package_bins) strcat(package_bins,"  ");
        strcat(package_bins,s);
    } else if (!strcmp(type,"COUNT")) snprintf(title,sizeof title,"安装清单包含 %d 个文件",atoi(s));
}
static void consume(Job *job,const char *data,int n) {
    for (int i=0;i<n;i++) {
        if (data[i]=='\n') {
            job->line[job->used]=0;
            if (!job->overlong) line(job,job->line);
            job->used=job->overlong=0;
        } else if (job->used<(int)sizeof job->line-1) job->line[job->used++]=data[i];
        else job->overlong=1;
    }
}
static int collect(Job *job) {
    if (!job->pid) return 0;
    char data[1024];
    for (;;) {
        if (lx_poll(job->fd,LX_READABLE,0)<=0) break;
        int n=lx_read(job->fd,data,sizeof data);
        if (n<=0) break;
        consume(job,data,n);
    }
    long status=console_call(51,(1UL<<32)|(unsigned)job->pid);
    if (status==-2) return 0;
    /* A child can exit between poll and wait. Drain its last bytes before closing. */
    int n;
    while ((n=lx_read(job->fd,data,sizeof data))>0) consume(job,data,n);
    lx_close(job->fd); job->pid=0;
    if (job==&hints) {
        if (hint_seed!=seed) { generate(); return 1; }
        if (!status && idea_count==3) {
            memcpy(ideas,suggestions,sizeof ideas);
            copy(origin,sizeof origin,"模型现场生成");
        }
    } else if (made && !status) {
        set_page(PROJECT); copy(title,sizeof title,brief);
        strcpy(current,"-"); strcpy(candidate,"-"); strcpy(previous,"-");
        command("send",key,brief,NULL); brief[0]=0; ui_text_begin(&editor,brief,0);
    } else if (!error_seen) copy(notice,sizeof notice,status ? "任务未完成，可在「我的软件」重新打开草稿" : "");
    return 1;
}

static void button(AppWindow *s,int id,int x,int y,int w,const char *name,int primary) {
    AppControl b={id,x,y-scroll,w,29,name};
    controls[ncontrols++]=b; app_button(s,&ctl,b,primary);
}
static void words(AppWindow *s,int x,int y,int width,const char *text,unsigned short ink) {
    label(s,x,y-scroll,width,text,ink);
}
static int paragraph(AppWindow *s,int y,int width,const char *text,unsigned short ink) {
    const char *p=text;
    while (*p && width>=16) {
        const char *start=p;
        int used=0;
        while (*p) {
            const char *next=p;
            int step=advance(utf8_next(&next));
            if (used+step>width) break;
            used+=step; p=next;
        }
        label(s,14,y-scroll,used,start,ink); y+=24;
    }
    return y;
}
static void input(AppWindow *s,int id,int y,int w) {
    AppControl b={id,14,y-scroll,w,29,""};
    controls[ncontrols++]=b;
    app_field(s,b,id==BRIEF ? &editor : &location,id==BRIEF ? brief : source,ctl.focus==id);
}
static void layout(AppWindow *s) {
    int w=s->width-28;
    ncontrols=0;
    app_rect(s,0,0,s->width,s->height,BG);
    app_rect(s,0,-scroll,s->width,43,INK);
    words(s,14,10,w,"LixinOS",WHITE);
    button(s,DISCOVER,s->width-158,7,68,"发现",page==HOME);
    button(s,LIB,s->width-86,7,74,"我的软件",page==LIBRARY || page==PACKAGE);
    if (page==HOME) {
        words(s,14,54,w,"这里的软件按需制作",INK);
        words(s,14,83,w,"让 Agent 按想法写应用，或从源码移植。通过测试后由你安装。",DIM);
        button(s,VIBE,14,120,w/2-4,"现场制作",1);
        button(s,PORT,18+w/2,120,w/2-4,"移植源码",0);
        words(s,14,174,w,"这次打开的灵感",INK);
        words(s,14,200,w,origin,DIM);
        button(s,REFRESH,s->width-96,173,82,"换一批",0);
        for (int i=0;i<3;i++) button(s,IDEA+i,14,238+i*42,w,ideas[i],0);
        content_h=368;
    } else if (page==FORM) {
        words(s,14,56,w,port ? "从现有源码移植" : "现场制作一个应用",INK);
        words(s,14,98,w,"描述应用，或在点子上加一笔",DIM);
        input(s,BRIEF,125,w);
        if (port) {
            words(s,14,177,w,"单文件 C 源码 · 系统绝对路径或 HTTPS 原始文件链接",DIM);
            input(s,SOURCE,206,w);
        }
        button(s,CREATE,14,port ? 258 : 182,146,"交给 Agent",1);
        content_h=port ? 308 : 233;
    } else if (page==LIBRARY) {
        words(s,14,56,w,"我的软件",INK);
        for (int i=0;i<item_count;i++) {
            int y=98+40*i;
            words(s,14,y+3,65,!strcmp(items[i].kind,"project") ? "作品" : "软件包",DIM);
            button(s,ITEM+i,88,y,w-74,items[i].name,0);
        }
        if (!total && !work.pid) words(s,14,100,w,"还没有作品或已登记的软件包",DIM);
        if (lib_page) button(s,PREV,14,306,76,"上一页",0);
        if ((lib_page+1)*5<total) button(s,NEXT,s->width-94,306,80,"下一页",0);
        content_h=348;
    } else if (page==PACKAGE) {
        words(s,14,62,w,pkg_name,INK);
        words(s,14,105,w,title,DIM);
        words(s,14,146,w,package_bins,DIM);
        words(s,14,194,w,"此包由系统安装器维护。",DIM);
        content_h=244;
    } else {
        words(s,14,56,w,title,INK);
        char status[200]; snprintf(status,sizeof status,"/work/store/%s",key);
        words(s,14,87,w,status,DIM);
        snprintf(status,sizeof status,"已安装 %.12s     通过测试 %.12s",current,candidate);
        words(s,14,119,w,status,INK);
        words(s,14,163,w,"下一步想怎样改？",DIM); input(s,BRIEF,190,w);
        button(s,SEND,14,234,102,"继续 Vibe",1);
        button(s,CHECK,124,234,82,"运行测试",0);
        if (strlen(candidate)==64 && strcmp(candidate,current)) button(s,PUBLISH,14,282,134,confirm==PUBLISH ? "确认安装" : "安装此版本",1);
        if (strlen(previous)==64) button(s,ROLLBACK,154,282,100,confirm==ROLLBACK ? "确认回滚" : "回滚",0);
        if (strlen(current)==64) button(s,REMOVE,14,324,134,confirm==REMOVE ? "确认卸载" : "卸载应用",0);
        content_h=366;
    }
    if (*notice) content_h=paragraph(s,content_h+2,w,notice,TEAL)+8;
}
static void activate(int id) {
    if (work.pid) return;
    if (id!=PUBLISH && id!=ROLLBACK && id!=REMOVE) confirm=0;
    if (id==DISCOVER) { set_page(HOME); notice[0]=0; }
    else if (id==LIB || id==PREV || id==NEXT) library(id==LIB ? 0 : lib_page+(id==NEXT ? 1 : -1));
    else if (id==REFRESH) generate();
    else if (id==VIBE || id==PORT || (id>=IDEA && id<IDEA+3)) {
        port=id==PORT; source[0]=0; notice[0]=0;
        copy(brief,sizeof brief,id>=IDEA ? ideas[id-IDEA] : "");
        ui_text_begin(&editor,brief,0); ui_text_begin(&location,source,0); set_page(FORM);
    } else if (id==CREATE && *brief && (!port || *source)) command("new",port ? "port" : "vibe",brief,source);
    else if (id==CREATE) copy(notice,sizeof notice,"请填好需求和所需的源码位置");
    else if (id>=ITEM && id<ITEM+item_count) {
        int i=id-ITEM; brief[0]=0; ui_text_begin(&editor,brief,0);
        if (!strcmp(items[i].kind,"project")) command("detail",items[i].key,NULL,NULL);
        else { set_page(PACKAGE); title[0]=package_bins[0]=0; copy(pkg_name,sizeof pkg_name,items[i].key); command("package",pkg_name,NULL,NULL); }
    } else if (id==SEND || id==CHECK) command(id==SEND ? "send" : "check",key,id==SEND ? brief : "",NULL);
    else if (id==PUBLISH || id==ROLLBACK || id==REMOVE) {
        if (confirm!=id) { confirm=id; return; }
        confirm=0;
        command(id==PUBLISH ? "publish" : id==ROLLBACK ? "rollback" : "remove",key,
                id==PUBLISH ? candidate : id==ROLLBACK ? previous : current,NULL);
    }
}
static int event(AppWindow *s,WindowEvent e) {
    if (e.type==WINDOW_CLIENT_CONFIGURE) { scroll=0; s->timer_ms=160; return 1; }
    if (e.type==WINDOW_CLIENT_TIMER) {
        int updated=collect(&hints); return collect(&work) || updated;
    }
    if (e.type==WINDOW_CLIENT_WHEEL) {
        scroll=ui_limit(scroll+(e.value>0 ? -36 : 36),content_h-s->height); return 1;
    }
    if (e.type==WINDOW_CLIENT_POINTER) {
        int hit=0;
        for (int i=0;i<ncontrols;i++) if (app_control_hit(controls[i],e.x,e.y)) {
            hit=controls[i].id;
            if (e.value && (hit==BRIEF || hit==SOURCE)) {
                UiText *t=hit==BRIEF ? &editor : &location;
                t->caret=t->anchor=app_field_position(controls[i],t,hit==BRIEF ? brief : source,e.x);
            }
            break;
        }
        int action=ui_pointer(&ctl,hit,e.value&1);
        if (action) activate(action);
        return 1;
    }
    if (e.type==WINDOW_CLIENT_KEY && e.value==9) {
        int ids[24]; for (int i=0;i<ncontrols;i++) ids[i]=controls[i].id;
        ctl.focus=ui_focus_next(ctl.focus,ids,ncontrols); return 1;
    }
    if (e.type==WINDOW_CLIENT_KEY && (e.value==13 || e.value==10)) {
        activate(ctl.focus==BRIEF || ctl.focus==SOURCE ? (page==FORM ? CREATE : SEND) : ctl.focus); return 1;
    }
    if ((e.type==WINDOW_CLIENT_KEY || e.type==WINDOW_CLIENT_PASTE) && (ctl.focus==BRIEF || ctl.focus==SOURCE) && !work.pid) {
        int isbrief=ctl.focus==BRIEF;
        UiText *t=isbrief ? &editor : &location; char *text=isbrief ? brief : source;
        int cap=isbrief ? sizeof brief : sizeof source;
        if (e.type==WINDOW_CLIENT_PASTE) return app_field_paste(s,t,text,cap,e.value);
        return app_field_key(s,t,text,cap,e.value);
    }
    return 0;
}
int main(void) {
    ctl.focus=DISCOVER;
    generate();
    int result=app_run3(event,layout);
    if (hints.pid) { console_call(55,hints.pid); lx_close(hints.fd); }
    if (work.pid) lx_close(work.fd);
    return result;
}
