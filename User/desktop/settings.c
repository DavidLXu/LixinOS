/* Settings is an ordinary adaptive desktop client. Each save changes one file. */
#include "window-widgets.h"
#include "../console.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { HOME_PAGE, LOGIN, DEVELOPMENT, AGENT, BOOT, SYSTEM, REPORT };
static const char *const titles[] = {
    "系统设置", "登录与终端", "开发环境", "Agent 设置", "启动与连接", "系统信息"
};
static const char *const hints[] = {
    "选择要调整的项目", "新终端生效", "新终端生效", "下次 Agent 任务生效",
    "下次启动生效", "选择项目查看当前系统"
};
static const char *const paths[] = {
    "", "/home/lixin/.profile", "/home/lixin/.profile", "/agent-policy/settings.py", "/.desktop-boot"
};
static const char *const zones[] = {"Asia/Shanghai", "UTC", "Asia/Tokyo", "Asia/Kolkata",
    "Europe/London", "Europe/Paris", "America/New_York", "America/Los_Angeles", "Australia/Sydney", NULL};
static const char *const modes[] = {"window", "split", NULL};
static const char *const editors[] = {"nano", "kilo", "dte", "slint-editor", NULL};
static const char *const toggles[] = {"0", "1", NULL};
static const char *const reports[] = {"内核与架构", "内存与系统", "处理器", "存储空间", "网络连接", "Mac 启动器"};
static const int report_ops[] = {4, 5, 8, 7, 0};

typedef struct {
    const char *key, *label, *initial;
} Field;
static const Field fields[] = {
    {"EDITOR", "默认编辑器", "nano"}, {"cd", "起始目录", "/home/lixin"},
    {"HOME", "主目录", "/home/lixin"}, {"PATH", "命令搜索路径", "/bin"},
    {"TMPDIR", "临时目录", ""}, {"LC_ALL", "语言环境", ""},
    {"COMPILER_PATH", "编译器搜索路径", ""}, {"RUSTC", "Rust 编译器", ""},
    {"CARGO_HOME", "Cargo 主目录", ""}, {"CARGO_BUILD_JOBS", "并行编译数", ""},
    {"CARGO_PROFILE_DEV_DEBUG", "开发调试信息", ""}, {"CARGO_INCREMENTAL", "增量编译", ""},
    {"CARGO_HTTP_CAINFO", "Cargo 证书", ""}, {"PYTHON_BASIC_REPL", "Python 基础终端", ""},
    {"_PYTHON_SYSCONFIGDATA_NAME", "Python 构建配置", ""},
    {"PIP_DISABLE_PIP_VERSION_CHECK", "跳过 pip 版本检查", ""},
    {"PIP_PROGRESS_BAR", "pip 进度条", ""}, {"PIP_ONLY_BINARY", "pip 二进制包", ""},
    {"PIP_NO_CACHE_DIR", "禁用 pip 缓存", ""}, {"PIP_NO_COMPILE", "跳过 pip 字节编译", ""},
    {"PIP_ROOT_USER_ACTION", "pip 管理员提示", ""}, {"SSL_CERT_FILE", "TLS 证书", ""}
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
enum { LIMIT = 32768, LINE_MAXIMUM = 512, HEAD = 34, ROW = 27, FOOT = 28 };
typedef struct {
    int line, start, end, stop; /* Byte offsets in the original, including untouched syntax. */
    int field, raw;
    char key[LINE_MAXIMUM + 1];
} Entry;
static Entry *entries;
static int count, capacity, page, selected, top, editing, loaded, existed;
static char original[LIMIT+1], working[LIMIT+1], draft[LINE_MAXIMUM+1], report[4097];
enum { FIELD=1, PRESET, SAVE, BACK, TRACK, FIRST_ROW=16 };
static UiControls controls;
static UiText input;
static int thumb_grab;
static const char *notice;
static int steps, preview, mode, report_id, port, kernel;
static char timezone[64], model[64];
static const char *const kernels[] = {"rust", "tcc", NULL};
static const char *const agent_labels[] = {"每次任务的工具次数", "结果预览字节数"};
static const char *const boot_labels[] = {"状态栏时区", "默认显示模式", "首选内核", "SSH 本机端口", "Agent 模型"};
static const char *const boot_hints[] = {
    "时区名称，如 Asia/Shanghai", "启动器未指定模式时使用", "内核菜单的默认选项",
    "SSH 模式使用，0 为自动分配", "auto 使用桥接默认模型"
};

static int file_read(const char *path, char *out, int *exists) {
    long size = lx_stat(path);
    if (size < LX_MISSING) {
        if ((!strcmp(path, paths[AGENT]) && lx_stat("/agent-policy") == LX_MISSING) ||
            (!strcmp(path, paths[LOGIN]) && (lx_stat("/home") == LX_MISSING ||
                                              lx_stat("/home/lixin") == LX_MISSING))) size = LX_MISSING;
    }
    *exists = size != LX_MISSING;
    out[0] = 0;
    if (!*exists) return 1;
    if (size < 0 || size > LIMIT) return 0;
    int fd = lx_open(path);
    if (fd < 0) return 0;
    int used = 0;
    while (used < size) {
        int n = lx_read(fd, out + used, size - used);
        if (n <= 0) break;
        used += n;
    }
    char extra;
    int ok = used == size && lx_read(fd, &extra, 1) == 0 && !memchr(out, 0, used);
    lx_close(fd);
    out[used] = 0;
    return ok;
}

static long batch(int op, const char *path, const char *other) {
    char *mail = (char *)0x443d7000;
    if (path) strcpy(mail, path);
    if (other) strcpy(mail+513, other);
    return console_call(45, op);
}

/* Compare and replace inside a generation-checked filesystem transaction. */
static int publish(const char *value) {
    const char *path = paths[page];
    char tmp[64], *mail = (char *)0x443d7000;
    unsigned nonce;
    if (lx_random(&nonce, sizeof nonce) < 0) return 0;
    snprintf(tmp, sizeof tmp, "/.settings-%d-%08x", lx_pid(), nonce);
    strcpy(mail, tmp);
    size_t size = strlen(value);
    memcpy((void *)0x443d8000, value, size);
    if (console_call(31, size) < 0) return 0;
    int exists, ok = 0;
    static char live[LIMIT+1];
    if (batch(0, NULL, NULL) >= 0) {
        if (!file_read(path, live, &exists) || exists != existed || strcmp(live, original))
            notice = "配置已变动，请返回后重新打开";
        else {
            int parents = 1;
            if (page == AGENT) parents = batch(257, "/agent-policy", NULL) >= 0;
            if (page == LOGIN || page == DEVELOPMENT)
                parents = batch(257, "/home", NULL) >= 0 && batch(257, "/home/lixin", NULL) >= 0;
            ok = parents && batch(5, tmp, path) >= 0 && batch(6, NULL, NULL) >= 0;
        }
    }
    if (!ok) {
        batch(7, NULL, NULL);
        unlink(tmp);
    }
    return ok;
}

static int lookup(const char *s, const char *const *options) {
    for (int i=0; options[i]; ++i) if (!strcmp(s, options[i])) return i;
    return -1;
}
static int field_id(const char *key) {
    for (int i=0; i<COUNT(fields); ++i) if (!strcmp(key, fields[i].key)) return i;
    return -1;
}
static int is_profile(void) { return page == LOGIN || page == DEVELOPMENT; }
static void add_entry(Entry e) {
    if (count == capacity) {
        int next = capacity ? capacity * 2 : 32;
        void *memory = realloc(entries, next * sizeof *entries);
        if (!memory) { loaded = 0; return; }
        entries = memory;
        capacity = next;
    }
    entries[count++] = e;
}
static int blank(char c) { return c == ' ' || c == '\t'; }
/* Keep quotes and expansions as entered. Multiple assignments and other complex
 * lines can be edited verbatim; adjacent comments and whitespace stay in place. */
static int word_end(const char *s, int p, int end) {
    char quote = 0;
    for (; p < end; ++p) {
        char c = s[p];
        if (!quote && blank(c)) break;
        if (!quote && strchr("|<>&;", c)) return -1;
        if (c == '\\' && quote != '\'' && ++p >= end) return -1;
        else if (c == quote) quote = 0;
        else if (!quote && (c == '\'' || c == '"')) quote = c;
    }
    return quote ? -1 : p;
}
static void profile_entries(void) {
    int seen[COUNT(fields)] = {0};
    int len = strlen(original);
    for (int line=0; line<len;) {
        int end = line;
        while (end<len && original[end]!='\n') ++end;
        Entry e = {.line=line, .start=line, .end=end, .stop=end, .field=-1, .raw=1};
        if (e.end>line && original[e.end-1]=='\r') --e.end;
        int p=line;
        line=end+1;
        while (p<e.end && blank(original[p])) ++p;
        if (p==e.end || original[p]=='#') continue;
        if (!strncmp(original+p,"export",6) && blank(original[p+6])) {
            p+=6;
            while (blank(original[p])) ++p;
        }
        int begin=p;
        while (isalnum((unsigned char)original[p]) || original[p]=='_') ++p;
        int cd = p-begin==2 && !strncmp(original+begin,"cd",2) && blank(original[p]);
        if (p>begin && p-begin<=LINE_MAXIMUM && (cd || original[p]=='=')) {
            memcpy(e.key, original+begin, p-begin);
            if (cd) while (blank(original[p])) ++p;
            else ++p;
            int stop=word_end(original,p,e.end);
            if (stop>=0) {
                int tail=stop;
                while (tail<e.end && blank(original[tail])) ++tail;
                if (tail==e.end || original[tail]=='#') {
                    e.raw=0; e.start=p; e.end=stop; e.field=field_id(e.key);
                    if (e.field>=0) seen[e.field]=1;
                }
            }
        }
        int section=e.field>=6 ? DEVELOPMENT : LOGIN;
        if (section==page) add_entry(e);
    }
    for (int i=0; i<COUNT(fields); ++i) {
        if (seen[i] || (i>=6 ? DEVELOPMENT : LOGIN)!=page) continue;
        Entry e = {.line=-1, .field=i};
        strcpy(e.key, fields[i].key);
        add_entry(e);
    }
    if (page==LOGIN) add_entry((Entry){.line=-1,.field=-1,.raw=1});
}

/* The package carries the host's IANA city list for validating typed zones. */
static int valid_zone(const char *s) {
    if (lookup(s,zones)>=0) return 1;
    if (!*s || strlen(s)>=sizeof timezone) return 0;
    static char list[LIMIT+1];
    int exists;
    if (!*list && !file_read("/lib/settings/zones",list,&exists)) return 0;
    char needle[68];
    snprintf(needle,sizeof needle,"\n%s\n",s);
    return !strchr(s,'\n') && strstr(list,needle)!=NULL;
}
static int valid_model(const char *s) {
    if (!*s || strlen(s)>=sizeof model) return 0;
    for (;*s;++s) if (!isalnum((unsigned char)*s) && !strchr("._-",*s)) return 0;
    return 1;
}
static void reload(void) {
    count=0; loaded=1;
    if (page == HOME_PAGE) { count=SYSTEM; return; }
    if (page == SYSTEM) { count=COUNT(reports); return; }
    loaded=file_read(paths[page], original, &existed);
    if (!loaded) { notice="无法读取配置，或文件超过 32 KiB"; return; }
    if (is_profile()) { profile_entries(); return; }
    count=2;
    int used=0;
    if (page==AGENT) {
        steps=50; preview=512;
        if (!existed) return;
        if (sscanf(original," print(\"%2d %4d\") %n",&steps,&preview,&used)==2 &&
            used==(int)strlen(original) && steps>=1 && steps<=60 && preview>=64 && preview<=2048) return;
    } else {
        char m[16], k[8];
        mode=kernel=0; port=2222; count=COUNT(boot_labels);
        strcpy(timezone,zones[0]); strcpy(model,"auto");
        if (!existed) return;
        if (sscanf(original,"%15s %63s %7s %5d %63s %n",m,timezone,k,&port,model,&used)==5 &&
            used==(int)strlen(original) && (mode=lookup(m,modes))>=0 && valid_zone(timezone) &&
            (kernel=lookup(k,kernels))>=0 && port>=0 && port<=65535 && valid_model(model)) return;
    }
    count=0; loaded=0; notice="配置格式无效，请检查源文件";
}
static const char *row_label(int i) {
    if (page==HOME_PAGE) return titles[i+1];
    if (page==SYSTEM) return reports[i];
    if (page==AGENT) return agent_labels[i];
    if (page==BOOT) return boot_labels[i];
    Entry *e=&entries[i];
    if (e->raw) return e->line<0 ? "+ 添加登录项" : "自定义语句";
    return e->field<0 ? e->key : fields[e->field].label;
}
static void row_value(int i, char out[LINE_MAXIMUM+1]) {
    out[0]=0;
    if (is_profile()) {
        Entry *e=&entries[i];
        if (e->line<0) {
            if (e->field>=0) strcpy(out,fields[e->field].initial);
        } else {
            int n=e->end-e->start;
            if (n>LINE_MAXIMUM) n=LINE_MAXIMUM;
            memcpy(out,original+e->start,n); out[n]=0;
        }
    } else if (page==AGENT) snprintf(out,LINE_MAXIMUM+1,"%d",i ? preview : steps);
    else if (page==BOOT) {
        if (i==3) snprintf(out,LINE_MAXIMUM+1,"%d",port);
        else strcpy(out,i==0 ? timezone : i==1 ? modes[mode] : i==2 ? kernels[kernel] : model);
    }
}

static const char *const *options(void) {
    if (page==BOOT) return selected==0 ? zones : selected==1 ? modes : selected==2 ? kernels : NULL;
    if (is_profile()) {
        Entry *e=&entries[selected];
        if (!e->raw && !strcmp(e->key,"EDITOR")) return editors;
        if (!e->raw && (!strcmp(e->key,"CARGO_INCREMENTAL") || !strcmp(e->key,"PYTHON_BASIC_REPL") ||
                        !strncmp(e->key,"PIP_NO_",7) || !strcmp(e->key,"PIP_DISABLE_PIP_VERSION_CHECK")))
            return toggles;
    }
    return NULL;
}
static const char *display_value(const char *s) {
    if (page==BOOT && selected==1) return !strcmp(s,"split") ? "双窗格" : "单窗格";
    return s;
}
static void enter_page(int next) {
    page=next; top=selected=editing=0; notice=NULL;
    reload();
}
static void activate(void) {
    if (!loaded || !count) return;
    if (page==HOME_PAGE) { enter_page(selected+1); return; }
    if (page==SYSTEM) {
        report_id=selected;
        long n=selected<COUNT(report_ops) ? console_call(29,report_ops[selected]) : 0;
        if (selected==COUNT(report_ops))
            strcpy(report,"在 Mac 上管理以下项目：\n工作盘与备份：workdisk.py\n启动磁盘：LIXINOS_DISK\n内核版本：run-kernel.sh --release\n模型密钥：钥匙串 LixinOS.DeepSeek\nSSH 密钥目录：LIXINOS_SSH_DIR");
        else if (n<0 || n>4096) strcpy(report,"当前设备不可用");
        else { memcpy(report,(char *)0x443d7000,n); report[n]=0; }
        page=REPORT; top=0; notice=NULL; return;
    }
    if (is_profile() && entries[selected].end-entries[selected].start>LINE_MAXIMUM) {
        notice="本行超过 Shell 的 512 字节限制"; return;
    }
    row_value(selected,draft);
    editing=1; controls.focus=FIELD; ui_text_begin(&input,draft,1); notice=NULL;
}
static void back(void) {
    if (editing) {
        editing=0; notice=NULL; reload();
        if (selected>=count) selected=count ? count-1 : 0;
    } else if (page==REPORT) {
        enter_page(SYSTEM); selected=report_id;
    } else if (page!=HOME_PAGE) {
        int previous=page; enter_page(HOME_PAGE); selected=previous-1;
    }
}
static int profile_change(void) {
    Entry *e=&entries[selected];
    char line[LINE_MAXIMUM+2];
    int n;
    if (!e->raw && word_end(draft,0,strlen(draft))!=(int)strlen(draft)) {
        notice="含空格的值请加引号"; return 0;
    }
    if (!e->raw && !strcmp(e->key,"cd") && !*draft) { notice="请输入起始目录"; return 0; }
    if (e->line>=0) {
        n=e->stop-e->line-(e->end-e->start)+(int)strlen(draft);
        if (n>LINE_MAXIMUM || strlen(original)-(e->end-e->start)+strlen(draft)>LIMIT) {
            notice="配置超过长度限制"; return 0;
        }
        memcpy(working,original,e->start);
        strcpy(working+e->start,draft);
        strcat(working,original+e->end);
        return 1;
    }
    if (e->raw) {
        if (!*draft) { notice="请输入变量赋值或 cd 命令"; return 0; }
        const char *p=draft;
        while (blank(*p)) ++p;
        int is_cd=!strncmp(p,"cd",2) && blank(p[2]);
        if (!is_cd && !strncmp(p,"export",6) && blank(p[6])) {
            p+=6;
            while (blank(*p)) ++p;
        }
        const char *name=p;
        while (isalnum((unsigned char)*p) || *p=='_') ++p;
        if (!is_cd && (p==name || isdigit((unsigned char)*name) || *p!='=')) {
            notice="使用 export NAME=value 或 cd 目录"; return 0;
        }
        n=snprintf(line,sizeof line,"%s",draft);
    } else if (!strcmp(e->key,"cd")) n=snprintf(line,sizeof line,"cd %s",draft);
    else n=snprintf(line,sizeof line,"export %s=%s",e->key,draft);
    size_t prefix=strlen(original);
    int separator=prefix && original[prefix-1]!='\n';
    if (n>LINE_MAXIMUM || prefix+separator+n+1>LIMIT) { notice="配置超过长度限制"; return 0; }
    strcpy(working,original);
    if (separator) strcat(working,"\n");
    strcat(working,line); strcat(working,"\n");
    return 1;
}
static void save(void) {
    notice=NULL;
    if (is_profile()) {
        if (!profile_change()) return;
    } else if (page==AGENT) {
        int lo=selected ? 64 : 1, hi=selected ? 2048 : 60;
        long v=strtol(draft,NULL,10);
        if (!*draft || draft[strspn(draft,"0123456789")] || v<lo || v>hi) {
            notice=selected ? "请输入 64 至 2048" : "请输入 1 至 60"; return;
        }
        snprintf(working,sizeof working,"print(\"%d %d\")\n",selected ? steps : (int)v,selected ? (int)v : preview);
    } else {
        if (selected==0 && !valid_zone(draft)) { notice="请输入有效的 IANA 城市时区"; return; }
        if (selected==3 && (!*draft || strlen(draft)>5 || draft[strspn(draft,"0123456789")] || atoi(draft)>65535)) {
            notice="端口范围为 0 至 65535"; return;
        }
        if (selected==4 && !valid_model(draft)) { notice="模型名支持字母、数字及 ._-"; return; }
        snprintf(working,sizeof working,"%s %s %s %d %s\n",
                 selected==1 ? draft : modes[mode], selected==0 ? draft : timezone,
                 selected==2 ? draft : kernels[kernel], selected==3 ? atoi(draft) : port,
                 selected==4 ? draft : model);
    }
    if (strcmp(original,working) && !publish(working)) {
        if (!notice) notice="保存失败，请检查空间并重试";
        return;
    }
    editing=0; reload();
    if (selected>=count) selected=count ? count-1 : 0;
    if (loaded) notice=page==BOOT ? "已保存，下次启动生效" : page==AGENT ? "已保存，下次任务生效" : "已保存，新终端生效";
}

enum { PAPER=0xf7be, WHITE=0xffff, INK=0x1947, MUTED=0x42ca, TEAL=0x1b8d, TINT=0xd730, EDGE=0xdedb, ERROR=0x8985 };
static int visible(AppWindow *s) {
    int n=(s->height-HEAD-FOOT)/ROW;
    return n<1 ? 1 : n;
}
static void reveal(AppWindow *s) {
    top=ui_list_reveal(top,selected,count,visible(s));
}
static int report_lines(AppWindow *s,int paint) {
    int row=0, cols=s->width-24;
    const char *p=report;
    while (*p) {
        const char *start=p;
        int used=0;
        while (*p && *p!='\n') {
            const char *next=p;
            int w=advance(utf8_next(&next));
            if (used && used+w>cols) break;
            used+=w; p=next;
        }
        if (paint && row>=top && row<top+visible(s)) {
            char fragment[4097];
            memcpy(fragment,start,p-start); fragment[p-start]=0;
            label(s,12,HEAD+(row-top)*ROW+2,cols,fragment,INK);
        }
        if (*p=='\n') ++p;
        ++row;
    }
    return row;
}
static int action_y(AppWindow *s) { return s->height-FOOT-30; }
static AppControl control(AppWindow *s,int id) {
    int y=action_y(s);
    if (id==SAVE) return (AppControl){id,s->width-72,y,62,25,"保存"};
    if (id==PRESET) return (AppControl){id,10,y,82,25,"切换选项"};
    if (id==FIELD) return (AppControl){id,10,HEAD+28,s->width-20,27,""};
    return (AppControl){BACK,5,4,35,25,"<"};
}
static int choices_only(void) { return page==BOOT && (selected==1 || selected==2); }
static void draw(AppWindow *s) {
    app_rect(s,0,0,s->width,s->height,PAPER);
    app_rect(s,0,0,s->width,HEAD,WHITE);
    int left=page==HOME_PAGE ? 12 : 48;
    if (page!=HOME_PAGE) app_button(s,&controls,control(s,BACK),0);
    const char *heading=editing ? row_label(selected) : page==REPORT ? reports[report_id] : titles[page];
    label(s,left,5,s->width-left-8,heading,INK);
    app_rect(s,0,HEAD-1,s->width,1,EDGE);
    if (page==REPORT) report_lines(s,1);
    else if (editing) {
        const char *hint=is_profile() ? (entries[selected].raw ? "登录语句" : entries[selected].key) :
            page==AGENT ? (selected ? "64 - 2048" : "1 - 60") : boot_hints[selected];
        label(s,12,HEAD+2,s->width-24,hint,MUTED);
        int y=HEAD+28;
        AppControl field=control(s,FIELD);
        if (choices_only()) {
            app_rect(s,field.x,field.y,field.w,field.h,WHITE);
            label(s,15,y+2,field.w-10,display_value(draft),INK);
            if (controls.focus==FIELD) app_rect(s,10,y+26,field.w,1,TEAL);
        } else app_field(s,field,&input,draft,controls.focus==FIELD);
        int buttons=action_y(s);
        if (options()) app_button(s,&controls,control(s,PRESET),0);
        app_button(s,&controls,control(s,SAVE),1);
        if (buttons>y+54 && !options()) label(s,12,y+32,s->width-24,"输入替换当前值，回车保存",MUTED);
    } else {
        for (int j=0;j<visible(s) && top+j<count;++j) {
            int i=top+j, y=HEAD+j*ROW;
            char value[LINE_MAXIMUM+1];
            row_value(i,value);
            if (is_profile() && entries[i].line<0 && !entries[i].raw) strcpy(value,"未配置");
            if (page==BOOT && i==1) strcpy(value,mode ? "双窗格" : "单窗格");
            int split=*value ? s->width*47/100 : s->width-23;
            int active=i==selected;
            app_rect(s,0,y,s->width,ROW-1,active ? TINT : WHITE);
            if (active) app_rect(s,0,y,3,ROW-1,TEAL);
            label(s,12,y+2,split-16,row_label(i),INK);
            if (*value) label(s,split,y+2,s->width-split-22,value,MUTED);
            label(s,s->width-17,y+2,15,">",TEAL);
        }
    }
    int fy=s->height-FOOT;
    app_rect(s,0,fy,s->width,FOOT,PAPER);
    app_rect(s,0,fy,s->width,1,EDGE);
    int items=page==REPORT ? report_lines(s,0) : count;
    int paged=!editing && items>visible(s);
    const char *hint=page==REPORT ? "当前系统" : hints[page];
    if (editing && !notice) hint=page==BOOT ? "显式启动参数优先" : "回车保存";
    label(s,9,fy+3,s->width-18,notice ? notice : hint,notice && editing ? ERROR : MUTED);
    if (paged) {
        UiThumb thumb=ui_list_thumb(top,items,visible(s),fy-HEAD);
        app_rect(s,s->width-7,HEAD,7,fy-HEAD,EDGE);
        app_rect(s,s->width-6,HEAD+thumb.start,5,thumb.length,MUTED);
    }
}
static void move(AppWindow *s,int delta) {
    notice=NULL;
    if (page==REPORT) {
        int limit=report_lines(s,0)-visible(s);
        top+=delta;
        if (top>limit) top=limit;
        if (top<0) top=0;
    } else if (count) {
        selected+=delta;
        if (selected>=count) selected=count-1;
        if (selected<0) selected=0;
        reveal(s);
    }
}
static void cycle(void) {
    const char *const *opts=options();
    if (!opts) return;
    int i=lookup(draft,opts)+1;
    strcpy(draft,opts[i] ? opts[i] : opts[0]);
    ui_text_begin(&input,draft,1); notice=NULL;
}
static int event(AppWindow *s,WindowEvent e) {
    if (e.type==WINDOW_CLIENT_CONFIGURE) {
        if (page==REPORT) move(s,0);
        else reveal(s);
        return 1;
    }
    int items=page==REPORT ? report_lines(s,0) : count;
    if (e.type==WINDOW_CLIENT_PASTE && editing && !choices_only() && controls.focus==FIELD)
        return app_field_paste(s,&input,draft,sizeof draft,e.value);
    if (e.type==WINDOW_CLIENT_WHEEL && !editing) {
        top=ui_list_top(top-e.value,items,visible(s));
        return 1;
    }
    if (e.type==WINDOW_CLIENT_POINTER) {
        int hit=0;
        if (page!=HOME_PAGE && app_control_hit(control(s,BACK),e.x,e.y)) hit=BACK;
        if (editing) {
            for (int id=FIELD;id<=SAVE;++id)
                if ((id!=PRESET || options()) && app_control_hit(control(s,id),e.x,e.y)) hit=id;
        } else if (e.y>=HEAD && e.y<s->height-FOOT && e.x>=0 && e.x<s->width) {
            int row=(e.y-HEAD)/ROW;
            if (items>visible(s) && e.x>=s->width-9) hit=TRACK;
            else if (page!=REPORT && row<visible(s) && top+row<count) hit=FIRST_ROW+top+row;
        }
        int press=e.value && !controls.down, was=controls.pressed;
        int action=ui_pointer(&controls,hit,e.value);
        if (controls.pressed==TRACK) {
            UiThumb thumb=ui_list_thumb(top,items,visible(s),s->height-FOOT-HEAD);
            int y=e.y-HEAD;
            if (press) thumb_grab=y>=thumb.start && y<thumb.start+thumb.length ? y-thumb.start : thumb.length/2;
            top=ui_list_drag(y-thumb_grab,thumb.travel,items,visible(s));
        }
        if (controls.pressed==FIELD && !choices_only()) {
            input.caret=app_field_position(control(s,FIELD),&input,draft,e.x);
            if (press) input.anchor=input.caret;
        }
        if (action==BACK) back();
        else if (action==SAVE) save();
        else if (action==PRESET || (action==FIELD && choices_only())) cycle();
        else if (action>=FIRST_ROW) { selected=action-FIRST_ROW; activate(); }
        return press || was || action;
    } else if (e.type==WINDOW_CLIENT_KEY) {
        int key=e.value;
        if (key==27) return -1;
        if (editing) {
            if (key==9) {
                int ids[]={FIELD,PRESET,SAVE,BACK};
                int plain[]={FIELD,SAVE,BACK};
                controls.focus=ui_focus_next(controls.focus,options()?ids:plain,options()?4:3);
            } else if (key==2) back();
            else if (key==13 || key==10 || (key==' ' && (controls.focus!=FIELD || choices_only()))) {
                if (controls.focus==BACK) back();
                else if (controls.focus==PRESET || (key==' ' && choices_only())) cycle();
                else save();
            } else if (controls.focus==FIELD && !choices_only()) {
                if (app_field_key(s,&input,draft,sizeof draft,key)) notice=NULL;
            }
        } else if (key=='j' || key==9 || key==303) move(s,1);
        else if (key=='k' || key==302) move(s,-1);
        else if (key==8 || key==127 || key==2) back();
        else if ((key==13 || key==10) && page!=REPORT) activate();
        else return 0;
    } else return 0;
    if (!editing && page!=REPORT) reveal(s);
    return 1;
}
int main(void) {
    enter_page(HOME_PAGE);
    int result=app_run3(event,draw);
    free(entries);
    return result;
}
