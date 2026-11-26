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
enum { LIMIT = 32768, LINE_MAXIMUM = 512 };
typedef struct {
    int line, start, end, stop; /* Byte offsets in the original, including untouched syntax. */
    int field, raw;
    char key[LINE_MAXIMUM + 1];
} Entry;
static Entry *entries;
static int count, capacity, page, selected, top, editing, loaded, existed;
static char original[LIMIT+1], working[LIMIT+1], draft[LINE_MAXIMUM+1], report[4097];
enum { FIELD=1, PRESET, SAVE, BACK, TRACK, NAV, FIRST_ROW=16 };
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

#include "settings-art.h"

#define RGB(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | ((b)>>3))
enum {
    CANVAS=RGB(248,247,245), RAIL=RGB(238,236,233), CARD=0xffff, INK=RGB(42,35,42),
    SECONDARY=RGB(92,83,92), LINE=RGB(220,215,212), ORANGE=RGB(185,68,30),
    PEACH=RGB(251,224,209), SOFT=RGB(255,246,237), PLUM=RGB(66,35,61), ERROR=RGB(170,45,48)
};
enum { SLIDERS, TERMINAL, CODE, SPARK, MONITOR, INFO, CLOCK, DISPLAY, CHIP, PORT, CUBE, FOLDER, PEN, NEXT, PREVIOUS };
static const unsigned short icon_ink[]={PLUM,RGB(69,83,111),RGB(117,68,139),RGB(141,64,110),
    ORANGE,RGB(32,99,112),RGB(157,83,36),RGB(71,95,153),RGB(115,73,134),
    RGB(27,105,107),RGB(138,70,116),RGB(154,108,40),RGB(78,98,151)};
static const unsigned short icon_bg[]={PEACH,RGB(227,231,242),RGB(237,227,246),RGB(248,227,239),
    PEACH,RGB(218,239,237),RGB(250,235,210),RGB(228,235,250),RGB(237,229,248),
    RGB(218,240,235),RGB(246,227,239),RGB(247,238,212),RGB(227,236,249)};
static int min_int(int a,int b) { return a<b?a:b; }
static unsigned short mix(unsigned short bg,unsigned short fg,int alpha) {
    int b=255-alpha;
    return ((((bg>>11)*b+(fg>>11)*alpha)/255)<<11) |
           (((((bg>>5)&63)*b+((fg>>5)&63)*alpha)/255)<<5) |
           (((bg&31)*b+(fg&31)*alpha)/255);
}
static void tint(AppWindow *s,int x,int y,unsigned short color,int alpha) {
    if (!alpha || (unsigned)x>=(unsigned)s->width || (unsigned)y>=(unsigned)s->height) return;
    unsigned short *p=s->pixels+y*s->width+x;
    *p=mix(*p,color,alpha);
}
static void symbol(AppWindow *s,int x,int y,int id,int size,unsigned short ink) {
    for (int j=0;j<size;j++)
        for (int i=0;i<size;i++)
            tint(s,x+i,y+j,ink,symbols[(j*24/size)*360+id*24+i*24/size]);
}
/* Render section headings and values with the desktop's proportional font atlas. */
static void text(AppWindow *s,int x,int y,int width,const char *str,int size,unsigned short ink) {
    int scale=size>16?2:1;
    while (*str && width>0) {
        const char *first=str;
        unsigned cp=utf8_next(&str);
        const unsigned char *mask=NULL;
        int han=han_slot(cp)>=0, step, stride, offset;
        if (han) {
            step=size; stride=16*scale; offset=0;
            mask=lx_pingfang_glyph(cp,scale);
        } else {
            int advance=lx_latin_glyph(cp,size>=20?2:0,scale,&mask);
            step=advance ? (advance*size+8*scale)/(16*scale) : 11;
            stride=20*scale; offset=2*scale;
        }
        if (step>width) break;
        if (mask) {
            for (int j=0;j<size;j++)
                for (int i=0;i<step;i++) {
                    int col=offset+i*16*scale/size;
                    if (col<stride) tint(s,x+i,y+j,ink,mask[j*16*scale/size*stride+col]);
                }
        } else {
            char glyph[8]={0}; memcpy(glyph,first,str-first);
            label(s,x,y-3,width,glyph,ink);
        }
        x+=step; width-=step;
    }
}
static int inset(int row,int h,int radius) {
    int dy=row<radius ? radius-row-1 : row>=h-radius ? row-(h-radius) : 0;
    int dx=radius;
    while (dx*dx+dy*dy>radius*radius) --dx;
    return radius-dx;
}
static void rounded(AppWindow *s,int x,int y,int w,int h,int radius,unsigned short fill) {
    radius=min_int(radius,min_int(w,h)/2);
    if (w<=0 || h<=0) return;
    for (int j=0;j<h;++j) {
        int edge=inset(j,h,radius);
        app_rect(s,x+edge,y+j,w-2*edge,1,fill);
    }
}
typedef struct { int rail,x,w,list,row,bottom,visible,nav_y,nav_h; } Layout;
static Layout layout(AppWindow *s) {
    Layout v={0};
    int small=s->height<330;
    v.rail=s->width>=570 && s->height>=245 && page!=HOME_PAGE ? 172 : 0;
    int margin=s->width<400 ? 10 : 18;
    v.w=min_int(s->width-v.rail-2*margin,700);
    v.x=v.rail+(s->width-v.rail-v.w)/2;
    v.list=small ? 70 : 122;
    v.row=small ? 34 : 48;
    v.bottom=s->height-(small ? 28 : 38);
    v.visible=(v.bottom-v.list)/v.row;
    if (v.visible<1) v.visible=1;
    v.nav_y=small?46:84; v.nav_h=small?34:44;
    return v;
}
static void reveal(AppWindow *s) {
    top=ui_list_reveal(top,selected,count,layout(s).visible);
}
static int report_lines(AppWindow *s,int paint) {
    Layout v=layout(s);
    int row=0,cols=v.w-32;
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
        if (paint && row>=top && row<top+v.visible) {
            char fragment[4097];
            memcpy(fragment,start,p-start); fragment[p-start]=0;
            label(s,v.x+12,v.list+(row-top)*v.row+(v.row-22)/2,cols,fragment,INK);
        }
        if (*p=='\n') ++p;
        ++row;
    }
    return row;
}
static AppControl control(AppWindow *s,int id) {
    Layout v=layout(s);
    int y=min_int(v.list+84,v.bottom-32);
    if (id==SAVE) return (AppControl){id,v.x+v.w-79,y,79,30,"保存"};
    if (id==PRESET) return (AppControl){id,v.x,y,94,30,"切换选项"};
    if (id==FIELD) return (AppControl){id,v.x+10,v.list+16,v.w-20,30,""};
    if (id>=NAV && id<NAV+SYSTEM) return (AppControl){id,9,v.nav_y+(id-NAV)*v.nav_h,v.rail-18,v.nav_h-4,titles[id-NAV+1]};
    return (AppControl){BACK,v.x+8,16,28,28,"返回"};
}
static int choices_only(void) { return page==BOOT && (selected==1 || selected==2); }
static int needs_back(Layout v) { return page!=HOME_PAGE && (!v.rail || editing || page==REPORT); }
static const char *release_name(void) {
    static char version[64];
    if (!version[0]) {
        long n=console_call(29,2);
        if (n>0 && n<(int)sizeof version) {
            memcpy(version,(void *)0x443d7000,n);
            version[strcspn(version,"\r\n")]=0;
        }
    }
    return strncmp(version,"kernel-",7)==0 ? version+7 : version;
}
static void sidebar(AppWindow *s,Layout v) {
    app_rect(s,0,0,v.rail,s->height,RAIL);
    app_rect(s,v.rail-1,0,1,s->height,LINE);
    int by=v.nav_y==46 ? 8 : 21;
    rounded(s,14,by,34,34,9,PLUM);
    symbol(s,19,by+5,SLIDERS,24,CARD);
    text(s,57,by+5,v.rail-60,"LixinOS",23,INK);
    int active=page==REPORT ? SYSTEM : page;
    for (int i=LOGIN;i<=SYSTEM;++i) {
        AppControl b=control(s,NAV+i-1);
        if (i==active || controls.pressed==b.id)
            rounded(s,b.x,b.y,b.w,b.h,8,i==active?PEACH:LINE);
        if (i==active) rounded(s,b.x+1,b.y+9,3,b.h-18,1,ORANGE);
        symbol(s,b.x+11,b.y+(b.h-22)/2,i,22,i==active?ORANGE:icon_ink[i]);
        text(s,b.x+44,b.y+(b.h-16)/2,b.w-46,b.name,16,i==active?PLUM:INK);
    }
    if (s->height-v.nav_y-SYSTEM*v.nav_h>60) {
        int y=s->height-52;
        app_rect(s,16,y,v.rail-32,1,LINE);
        text(s,17,y+12,v.rail-30,release_name(),13,SECONDARY);
    }
}
static void heading(AppWindow *s,Layout v) {
    int y=10,h=v.list-22;
    for (int j=0;j<h && y+j<s->height;j++) {
        int edge=inset(j,h,10);
        for (int i=edge;i<v.w-edge;++i) {
            int x=v.x+i;
            if ((unsigned)x<(unsigned)s->width)
                s->pixels[(y+j)*s->width+x]=skyline[(j*96/h)*640+i*640/v.w];
        }
    }
    int back=needs_back(v), x=v.x+18;
    if (back) {
        AppControl b=control(s,BACK);
        rounded(s,b.x,b.y,b.w,b.h,8,PLUM);
        symbol(s,b.x+5,b.y+5,PREVIOUS,18,CARD);
        x=b.x+b.w+9;
    }
    const char *title=editing?row_label(selected):page==REPORT?reports[report_id]:titles[page];
    const char *sub=editing ? (is_profile() ? (entries[selected].raw?"登录语句":entries[selected].key) :
        page==AGENT ? (selected?"64 – 2048 字节":"1 – 60 次") : boot_hints[selected]) :
        page==REPORT ? titles[SYSTEM] : hints[page];
    int tall=h>=70,room=v.x+v.w-x-10;
    text(s,x,y+(tall?18:8),room,title,tall?25:18,CARD);
    if (tall) text(s,x,y+55,room,sub,15,RGB(254,220,208));
    else if (editing) text(s,x,y+29,room,sub,13,CARD);
}
static int row_icon(int i) {
    static const int boot[]={CLOCK,DISPLAY,CHIP,PORT,CUBE};
    static const int system[]={CHIP,MONITOR,CHIP,CUBE,PORT,TERMINAL};
    if (page==BOOT) return boot[i];
    if (page==SYSTEM) return system[i];
    if (page==HOME_PAGE) return i+1;
    if (page==AGENT) return i?MONITOR:SPARK;
    if (page==LOGIN) return entries[i].field==0?PEN:entries[i].field>=1 && entries[i].field<=4?FOLDER:TERMINAL;
    return CODE;
}
static void draw_rows(AppWindow *s,Layout v) {
    int n=min_int(count-top,v.visible);
    if (n<=0) return;
    rounded(s,v.x,v.list,v.w,n*v.row,10,LINE);
    for (int j=0;j<n;j++) {
        int i=top+j, y=v.list+j*v.row, chosen=i==selected;
        unsigned short bg=chosen ? SOFT : CARD;
        for (int k=1;k<v.row;++k) {
            int edge=inset(j*v.row+k,n*v.row,10);
            app_rect(s,v.x+edge+1,y+k,v.w-2*edge-2,1,bg);
        }
        int icon=row_icon(i), mid=y+v.row/2;
        int badge=v.row>=40 ? 32 : 26;
        rounded(s,v.x+10,mid-badge/2,badge,badge,8,icon_bg[icon]);
        symbol(s,v.x+10+(badge-20)/2,mid-10,icon,20,icon_ink[icon]);
        if (chosen) app_rect(s,v.x+1,mid-7,3,14,ORANGE);
        char value[LINE_MAXIMUM+1];
        row_value(i,value);
        if (is_profile() && entries[i].line<0 && !entries[i].raw) strcpy(value,"未配置");
        if (page==BOOT && i==1) strcpy(value,mode?"双窗格":"单窗格");
        int x=v.x+badge+20, end=v.x+v.w-32,split=v.x+v.w*53/100;
        text(s,x,mid-8,(*value ? split-8 : end)-x,row_label(i),16,INK);
        if (*value) text(s,split,mid-8,end-split,value,15,SECONDARY);
        symbol(s,end+6,mid-8,NEXT,16,SECONDARY);
    }
}
static void draw_button(AppWindow *s,AppControl b,int primary) {
    unsigned short bg=primary ? ORANGE : (controls.focus==b.id ? PEACH : LINE);
    if (controls.pressed==b.id) bg=primary?PLUM:PEACH;
    rounded(s,b.x,b.y,b.w,b.h,8,bg);
    text(s,b.x+(primary?24:16),b.y+7,b.w-18,b.name,16,primary?CARD:INK);
    if (controls.focus==b.id) app_rect(s,b.x+10,b.y+b.h-4,b.w-20,2,primary?CARD:ORANGE);
}
static void draw(AppWindow *s) {
    Layout v=layout(s);
    app_rect(s,0,0,s->width,s->height,CANVAS);
    if (v.rail) sidebar(s,v);
    heading(s,v);
    if (editing) {
        AppControl field=control(s,FIELD);
        rounded(s,v.x,v.list,v.w,58,9,LINE);
        rounded(s,v.x+1,v.list+1,v.w-2,56,8,CARD);
        if (choices_only()) {
            text(s,field.x+4,field.y+3,field.w-32,display_value(draft),16,INK);
            symbol(s,field.x+field.w-22,field.y+2,NEXT,18,ORANGE);
        } else app_field(s,field,&input,draft,controls.focus==FIELD);
        if (controls.focus==FIELD) app_rect(s,field.x,field.y+field.h-1,field.w,2,ORANGE);
        if (options()) draw_button(s,control(s,PRESET),0);
        draw_button(s,control(s,SAVE),1);
    } else if (page==REPORT) {
        rounded(s,v.x,v.list,v.w,v.visible*v.row,10,CARD);
        report_lines(s,1);
    } else draw_rows(s,v);
    if (notice) text(s,v.x+4,s->height-24,v.w-8,notice,15,editing?ERROR:SECONDARY);
    int items=page==REPORT ? report_lines(s,0) : count;
    if (!editing && items>v.visible) {
        UiThumb thumb=ui_list_thumb(top,items,v.visible,v.bottom-v.list);
        rounded(s,v.x+v.w+3,v.list+thumb.start,4,thumb.length,2,SECONDARY);
    }
}
static void move(AppWindow *s,int delta) {
    notice=NULL;
    if (page==REPORT) {
        int limit=report_lines(s,0)-layout(s).visible;
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
    Layout v=layout(s);
    if (e.type==WINDOW_CLIENT_CONFIGURE) {
        static int opened;
        if (!opened) {
            opened=1;
            if (s->width>=570 && s->height>=245) enter_page(BOOT);
        }
        if (page==REPORT) move(s,0);
        else reveal(s);
        return 1;
    }
    int items=page==REPORT ? report_lines(s,0) : count;
    if (e.type==WINDOW_CLIENT_PASTE && editing && !choices_only() && controls.focus==FIELD)
        return app_field_paste(s,&input,draft,sizeof draft,e.value);
    if (e.type==WINDOW_CLIENT_WHEEL && !editing) {
        top=ui_list_top(top-e.value,items,v.visible);
        return 1;
    }
    if (e.type==WINDOW_CLIENT_POINTER) {
        int hit=0;
        if (v.rail) {
            for (int i=0;i<SYSTEM;++i)
                if (app_control_hit(control(s,NAV+i),e.x,e.y)) hit=NAV+i;
        }
        if (needs_back(v) && app_control_hit(control(s,BACK),e.x,e.y)) hit=BACK;
        if (editing) {
            for (int id=FIELD;id<=SAVE;++id)
                if ((id!=PRESET || options()) && app_control_hit(control(s,id),e.x,e.y)) hit=id;
        } else if (e.y>=v.list && e.y<v.bottom && e.x>=v.x && e.x<v.x+v.w+10) {
            int row=(e.y-v.list)/v.row;
            if (items>v.visible && e.x>=v.x+v.w) hit=TRACK;
            else if (page!=REPORT && e.x<v.x+v.w && row<v.visible && top+row<count) hit=FIRST_ROW+top+row;
        }
        int press=e.value && !controls.down, was=controls.pressed;
        int action=ui_pointer(&controls,hit,e.value);
        if (controls.pressed==TRACK) {
            UiThumb thumb=ui_list_thumb(top,items,v.visible,v.bottom-v.list);
            int y=e.y-v.list;
            if (press) thumb_grab=y>=thumb.start && y<thumb.start+thumb.length ? y-thumb.start : thumb.length/2;
            top=ui_list_drag(y-thumb_grab,thumb.travel,items,v.visible);
        }
        if (controls.pressed==FIELD && !choices_only()) {
            input.caret=app_field_position(control(s,FIELD),&input,draft,e.x);
            if (press) input.anchor=input.caret;
        }
        if (action==BACK) back();
        else if (action==SAVE) save();
        else if (action==PRESET || (action==FIELD && choices_only())) cycle();
        else if (action>=NAV && action<NAV+SYSTEM) enter_page(LOGIN+action-NAV);
        else if (action>=FIRST_ROW) { selected=action-FIRST_ROW; activate(); }
        if (action && !editing && page!=REPORT) reveal(s);
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
