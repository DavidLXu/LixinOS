// LixinOS Bash. Build inside LixinOS: tcc /src/bash/bash.c -o /bin/bash
#include <stdio.h>
#include <lixin.h>
long lx_console_call(long operation, long value);

typedef unsigned long Size;
Size strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, Size);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strcpy(char *, const char *);
char *strdup(const char *);
void *memcpy(void *, const void *, Size);
void *calloc(Size, Size);
void *realloc(void *, Size);
void free(void *);
void abort(void);
void qsort(void *, Size, Size, int (*)(const void *, const void *));

enum { STAT, HISTORY_GET, READ_DIRECTORY, HISTORY_APPEND, SPAWN_BEGIN, SPAWN_ARG,
       UNUSED, CHILD_STATUS, READ_STREAM, CLEAR_STREAM, APPEND_STREAM, STORE_STREAM,
       RESERVED, LAST_ERROR, CHDIR, PANE_READY, NEXT_TERMINAL, CLOSE_TERMINAL, START_JOB, WAIT_JOB, RESET_PIPE, JOB_OUTPUT, JOB_INPUT };
enum { WORD, PIPE, READ, WRITE, APPEND, BACKGROUND };
#define LINE 512
#define WORD_MAX 4096
#define MAIL ((char *)0x443d8000)
#ifndef BASH_PROMPT
#define BASH_PROMPT "lixin:%s$ "
#endif

typedef struct { int kind; char *text; } Token;
typedef struct { Token items[LINE+3]; int count; } Tokens;
typedef struct { char *name, *value; int exported; } Variable;
typedef struct Session {
    int id, status, vars, histories, position, repeated_tab, skip_lf, explicit_env;
    char cwd[LINE+1], previous_cwd[LINE+1], line[LINE+1], draft[LINE+1];
    Variable variables[64];
    char *history[64];
    struct Session *next;
} Session;
typedef struct Terminal {
    int id, next_id;
    Session *first, *active;
    struct Terminal *next;
} Terminal;

static const char help[]=
    "Shell: cd command edit exit export help history shell type unset\n"
    "Files: basename cat cp dirname find ls mkdir mv pwd rm rmdir stat touch tree\n"
    "Text: cmp cut diff echo grep head nl printf sed seq sort tail tee test tr uniq wc xargs\n"
    "System: clear cpuinfo date df du false htop netinfo ps sleep sysinfo true uname\n"
    "Apps: agent bash cecho desktop git kilo links lust nano paint python sh ssh tcc\n"
    "Power: q shutdown\n";
static const char shell_identity[] = "LixinOS Bash (C shell)";
static char error[256];
static int fail(const char *message) {
    snprintf(error, sizeof(error), "%s", message);
    return -1;
}
static void *checked(void *p) {
    if (!p) { puts("Bash: out of memory"); abort(); }
    return p;
}
static char *copy(const char *s) { return checked(strdup(s)); }
static int equal(const char *a, const char *b) { return strcmp(a,b)==0; }
static int prefix(const char *s, const char *p) { return strncmp(s,p,strlen(p))==0; }
static int digit(int c) { return c>='0' && c<='9'; }
static int namechar(int c) {
    return digit(c) || c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z');
}
static int shell_builtin(const char *name) {
    return equal(name,"history") || equal(name,"type") || equal(name,"command")
        || equal(name,"unset") || equal(name,"exit") || equal(name,"export");
}
static int request(int op, unsigned index, const char *s) {
    Size n=strlen(s);
    if (n>WORD_MAX) return -1;
    memcpy(MAIL,s,n+1);
    return lx_process_call(op,index);
}
static char *query(int op, unsigned index, const char *s) {
    int n=request(op,index,s);
    if (n<0 || n>WORD_MAX) return 0;
    MAIL[n]=0;
    return MAIL;
}
static int service(int op, unsigned index, const char *s) {
    if (request(op,index,s)>=0) return 0;
    char *msg=query(LAST_ERROR,0,"");
    return fail(msg ? msg : "process request failed");
}
static int var_index(Session *s, const char *key) {
    for (int i=0;i<s->vars;i++) if (equal(key,s->variables[i].name)) return i;
    return -1;
}
static const char *value(Session *s, const char *key) {
    if (equal(key,"PWD")) return s->cwd;
    int i=var_index(s,key);
    return i<0 ? "" : s->variables[i].value;
}
static int assign(Session *s, const char *key, const char *val) {
    int i=var_index(s,key);
    if (i<0) {
        if (s->vars==64) return fail("too many variables");
        i=s->vars++;
        s->variables[i].name=copy(key);
        s->variables[i].exported=0;
        s->variables[i].value=0;
    }
    free(s->variables[i].value);
    s->variables[i].value=copy(val);
    return 0;
}
static int binding(Session *s,char *arg,int exported) {
    char *eq=strchr(arg,'=');
    if (eq) *eq=0;
    if (!*arg || digit(*arg) || equal(arg,"PWD")) return fail("invalid variable name");
    for (char *c=arg;*c;c++) if (!namechar(*c)) return fail("invalid variable name");
    if (eq || var_index(s,arg)<0) if (assign(s,arg,eq ? eq+1 : "")<0) return -1;
    if (exported) { s->explicit_env=1; s->variables[var_index(s,arg)].exported=1; }
    return 0;
}
static int environment(Session *s) {
    int used=0;
    for (int i=0;i<s->vars;i++) {
        Variable *v=&s->variables[i];
        if (!v->exported) continue;
        int n=snprintf(MAIL+used,8192-used,"%s=%s",v->name,v->value);
        if (n<0 || n>=8192-used) return fail("environment exceeds 8 KiB");
        used+=n+1;
    }
    lx_console_call(56,used || s->explicit_env ? used : -1); /* Older kernels use launcher defaults. */
    return 0;
}
static Session *new_session(int id,const char *cwd) {
    Session *s=checked(calloc(1,sizeof(*s)));
    s->id=id;
    strcpy(s->cwd,cwd);
    assign(s,"PATH","/bin");
    long env=lx_console_call(53,0);
    s->explicit_env=env>0;
    if (env>1 && env<=8193) {
        char *end=MAIL+env-1;
        for (char *p=MAIL; p<end;) {
            char *next=p+strlen(p)+1;
            if (next>end) break;
            binding(s,p,1); p=next;
        }
    }
    return s;
}
static void drop_session(Session *s) {
    for (int i=0;i<s->vars;i++) { free(s->variables[i].name); free(s->variables[i].value); }
    for (int i=0;i<s->histories;i++) free(s->history[i]);
    free(s);
}
static void remember(Session *s,const char *line) {
    if (s->histories==64) {
        free(s->history[0]);
        for (int i=1;i<64;i++) s->history[i-1]=s->history[i];
        s->histories--;
    }
    s->history[s->histories++]=copy(line);
}
static void prompt(Session *s) {
    int color=(lx_geometry()>>32)!=0;
    if (color) printf("\033[1;36m");
    printf(BASH_PROMPT,s->cwd);
    if (color) printf("\033[0m");
}
static void clear_tokens(Tokens *t) {
    while (t->count) free(t->items[--t->count].text);
}
static void token(Tokens *t,int kind,const char *s) {
    t->items[t->count].kind=kind;
    t->items[t->count++].text=s ? copy(s) : 0;
}
// With cursor set, leave an unfinished quote open and emit the final argument
// even after whitespace. Completion uses the same expansions as execution.
static int lex(Session *s,const char *line,Tokens *t,int cursor,int *open_quote) {
    char word[WORD_MAX+1], key[LINE+1], number[16];
    int n=0, started=0, quote=0;
    while (*line) {
        int c=*line++;
        if (!quote && (c==' ' || c=='\t' || strchr("|<>&",c))) {
            if (started) { word[n]=0; token(t,WORD,word); n=started=0; }
            if (c=='&') token(t,BACKGROUND,0);
            if (c=='|') token(t,PIPE,0);
            if (c=='<') token(t,READ,0);
            if (c=='>') {
                if (*line=='>') { line++; token(t,APPEND,0); }
                else token(t,WRITE,0);
            }
            continue;
        }
        if (!quote && c=='#' && !started) {
            if (cursor) return -1;
            break;
        }
        started=1;
        if (quote==c) { quote=0; continue; }
        if (!quote && (c==39 || c=='"')) { quote=c; continue; }
        if (c=='\\' && quote!=39) {
            if (!*line) return fail("unfinished escape");
            if (quote=='"' && !strchr("\"$`\\",*line)) {
                if (n==WORD_MAX) return fail("word too long");
                word[n++]='\\';
            }
            c=*line++;
        } else if (c=='$' && quote!=39 && (*line=='?' || namechar(*line))) {
            const char *v;
            if (*line=='?') { line++; snprintf(number,sizeof(number),"%d",s->status); v=number; }
            else {
                int k=0; while (namechar(*line)) key[k++]=*line++;
                key[k]=0; v=value(s,key);
            }
            Size size=strlen(v);
            if (size>WORD_MAX-n) return fail("word too long");
            memcpy(word+n,v,size); n+=size;
            continue;
        } else if (!quote && (c==';' || c=='&')) return fail("unsupported operator");
        if (n==WORD_MAX) return fail("word too long");
        word[n++]=c;
    }
    if (!cursor && quote) return fail("unclosed quote");
    if (started || cursor) { word[n]=0; token(t,WORD,word); }
    if (open_quote) *open_quote=quote;
    return 0;
}
static const char *path_dir(const char *part,char *dir) {
    const char *end=strchr(part,':');
    Size n=end ? (Size)(end-part) : strlen(part);
    if (!n) strcpy(dir,".");
    else if (n>LINE) *dir=0;
    else { memcpy(dir,part,n); dir[n]=0; }
    return end ? end+1 : 0;
}
static int program(Session *s,const char *name,char *path,char *kind) {
    if (equal(name,"edit") && *value(s,"EDITOR")) name=value(s,"EDITOR");
    if (equal(name,"q")) name="shutdown";
    int explicit_path=strchr(name,'/')!=0;
    const char *part=value(s,"PATH");
    do {
        char dir[LINE+1];
        if (explicit_path) {
            if (strlen(name)>LINE) break;
            strcpy(path,name); part=0;
        } else {
            part=path_dir(part,dir);
            if (!*dir || snprintf(path,LINE+1,"%s/%s",dir,name)>LINE) continue;
        }
        char *k=query(STAT,0,path);
        if (k && (*k=='@' || equal(k,"x"))) { strcpy(kind,k); return 0; }
        if (explicit_path) return fail(!k ? "file not found" : equal(k,"d") ? "is a directory" : "invalid LixinOS executable");
    } while (part);
    return fail(explicit_path ? "file not found" : "unknown command");
}
static int skip_history(Session *s,Tokens *t) {
    if (!t->count) return 1;
    Token *a=t->items;
    if (a[0].kind!=WORD) return 0;
    int inspect=t->count>=2 && a[1].kind==WORD && equal(a[1].text,"inspect");
    if (t->count!=1 && !inspect) return 0;
    char path[LINE+1],kind[64];
    if (program(s,a[0].text,path,kind)<0) return 0;
    return (t->count==1 && equal(kind,"@shutdown")) || (inspect && equal(kind,"@agent"));
}

typedef struct { char *args[LINE+1], *in, *out; int argc, append; } Stage;
static int parse_stage(Tokens *t,int start,int end,Stage *st) {
    st->argc=0; st->in=st->out=0; st->append=0;
    for (int i=start;i<end;i++) {
        Token item=t->items[i];
        if (item.kind==WORD) st->args[st->argc++]=item.text;
        else {
            if (++i>=end || t->items[i].kind!=WORD) return fail("expected filename");
            if (item.kind==READ) st->in=t->items[i].text;
            else { st->out=t->items[i].text; st->append=item.kind==APPEND; }
        }
    }
    return st->argc ? 0 : fail("missing command");
}
static int run_jobs(Session *s,Tokens *t,int background) {
    Stage st;
    int jobs[LINE+1], count=0, result=0;
    for (int start=0;start<t->count;) {
        int end=start;
        while (end<t->count && t->items[end].kind!=PIPE) end++;
        if (parse_stage(t,start,end,&st)<0) {result=-1;break;}
        char path[LINE+1],kind[64];
        int local=equal(st.args[0],"help") || equal(st.args[0],"cd") || strchr(st.args[0],'=')!=0;
        if (program(s,local ? "/bin/bash" : st.args[0],path,kind)<0) {result=-1;break;}
        if (st.in && service(JOB_INPUT,0,st.in)<0) {result=-1;break;}
        if (st.out && service(JOB_OUTPUT,st.append,st.out)<0) {result=-1;break;}
        if (service(SPAWN_BEGIN,st.in!=0,path)<0) {result=-1;break;}
        if (local && (service(SPAWN_ARG,0,"bash")<0 || service(SPAWN_ARG,0,"--stage-builtin")<0)) {result=-1;break;}
        for (int i=0;i<st.argc;i++) {
            if (service(SPAWN_ARG,0,st.args[i])<0) {result=-1;break;}
        }
        if (result<0) break;
        int id=request(START_JOB,2*(end<t->count)+4*background,"");
        if (id<0) {
            char *why=query(LAST_ERROR,0,"");
            result=fail(why ? why : "could not start process");
            break;
        }
        jobs[count++]=id;
        start=end+1;
    }
    request(RESET_PIPE,0,"");
    if (!background) {
        for (int i=0;i<count;i++) {
            s->status=request(WAIT_JOB,jobs[i],"");
            if (s->status<0 && !result) {
                char *why=query(LAST_ERROR,0,"");
                result=fail(why ? why : "process failed");
            }
        }
    } else if (count) printf("[pid %d]\n",jobs[count-1]);
    return result;
}
static int execute(Session *s,Tokens *t) {
    if (!t->count) return 0;
    int background=t->items[t->count-1].kind==BACKGROUND;
    if (background) t->count--;
    if (!t->count) return fail("missing background command");
    int concurrent=background, pipeline=0,builtin=0;
    for (int i=0;i<t->count;i++) {
        if (t->items[i].kind==BACKGROUND) return fail("expected & at end of command");
        if (t->items[i].kind==PIPE) pipeline=concurrent=1;
    }
    Stage st;
    // Validate the entire pipeline before a stage can modify the filesystem.
    for (int start=0,i=0;i<=t->count;i++) {
        if (i<t->count && t->items[i].kind!=PIPE) continue;
        if (parse_stage(t,start,i,&st)<0) return -1;
        if (shell_builtin(st.args[0])) {
            if (pipeline || background || st.in || st.out)
                return fail("this shell builtin requires a standalone command");
            const char *name=st.args[0];
            const char *operand=st.argc>1 ? st.args[1] : 0;
            if (equal(name,"exit")) {
                int status=s->status;
                if (st.argc>2) return fail("usage: exit [STATUS]");
                if (operand) {
                    if (!*operand) return fail("expected a numeric exit status");
                    status=0;
                    for (const char *p=operand; *p; ++p) {
                        if (!digit(*p)) return fail("expected a numeric exit status");
                        status=(10*status+(*p-'0'))%256;
                    }
                }
                s->status=status;
                return 1;
            }
            s->status=0;
            if (equal(name,"export")) {
                if (!operand) return fail("usage: export NAME[=VALUE] ...");
                for (int j=1;j<st.argc;j++) if (binding(s,st.args[j],1)<0) return -1;
            } else if (equal(name,"history")) {
                if (operand) return fail("usage: history");
                for (int j=0;j<s->histories;++j) printf("%d  %s\n",j+1,s->history[j]);
            } else if (equal(name,"unset")) {
                if (!operand) return fail("usage: unset NAME ...");
                for (int j=1;j<st.argc;++j) {
                    int at=var_index(s,st.args[j]);
                    if (at<0) continue;
                    free(s->variables[at].name);
                    free(s->variables[at].value);
                    s->variables[at]=s->variables[--s->vars];
                }
            } else {
                int show_path=equal(name,"command");
                int first=show_path ? 2 : 1;
                if (st.argc<=first || (show_path && !equal(operand,"-v")))
                    return fail(show_path ? "usage: command -v NAME ..." : "usage: type NAME ...");
                for (int j=first;j<st.argc;++j) {
                    const char *target=st.args[j];
                    char path[LINE+1],kind[64];
                    if (shell_builtin(target) || equal(target,"cd") || equal(target,"help") || equal(target,"shell")) {
                        if (show_path) puts(target); else printf("%s is a shell builtin\n",target);
                    } else if (program(s,target,path,kind)<0) {
                        printf("%s: command not found\n",target);
                        s->status=1;
                    } else if (show_path) puts(path);
                    else printf("%s is %s\n",target,path);
                }
            }
            return 0;
        }
        if (st.in || st.out) concurrent=1;
        if (equal(st.args[0],"cd") || equal(st.args[0],"help") || strchr(st.args[0],'=')) builtin=1;
        start=i+1;
    }
    if (environment(s)<0) return -1;
    if (pipeline || background || (concurrent && !builtin)) return run_jobs(s,t,background);
    if (service(CLEAR_STREAM,0,"")<0) return -1;
    for (int start=0;start<t->count;) {
        int end=start;
        while (end<t->count && t->items[end].kind!=PIPE) end++;
        parse_stage(t,start,end,&st);
        int capture=end<t->count || st.out, stream=start!=0 || st.in;
        if (st.in && service(READ_STREAM,0,st.in)<0) return -1;
        if (st.out) { char *k=query(STAT,0,st.out); if (k && equal(k,"d")) return fail("is a directory"); }
        char *name=st.args[0];
        s->status=0;
        char *eq=strchr(name,'='), *slash=strchr(name,'/');
        if (eq && (!slash || slash>eq)) {
            if (st.argc!=1 || binding(s,name,0)<0 || service(CLEAR_STREAM,0,"")<0) return -1;
        } else {
            char path[LINE+1],kind[64];
            if (program(s,name,path,kind)<0) return -1;
            if (equal(kind,"@cd")) {
                if (st.argc>2) return fail("expected one argument");
                int previous=st.argc==2 && equal(st.args[1],"-");
                if (previous && !*s->previous_cwd) return fail("no previous directory");
                const char *target=previous ? s->previous_cwd : st.argc==2 ? st.args[1] : *value(s,"HOME") ? value(s,"HOME") : "/";
                char *cwd=query(CHDIR,0,target);
                if (!cwd) return fail("directory not found");
                strcpy(s->previous_cwd,s->cwd);
                strcpy(s->cwd,cwd);
                if (previous) {
                    if (capture) {
                        char line[LINE+2];
                        snprintf(line,sizeof(line),"%s\n",s->cwd);
                        if (service(APPEND_STREAM,0,line)<0) return -1;
                    } else puts(s->cwd);
                }
                if (service(CLEAR_STREAM,0,"")<0) return -1;
            } else if (equal(kind,"@help")) {
                if (service(CLEAR_STREAM,0,"")<0) return -1;
                if (capture) { if (service(APPEND_STREAM,0,help)<0) return -1; }
                else printf("%s",help);
            } else {
                if ((capture || stream) && kind[0]=='@' && !equal(kind,"@touch") && !equal(kind,"@mkdir") && !equal(kind,"@rm") && !equal(kind,"@rmdir") && !equal(kind,"@cp") && !equal(kind,"@mv") && !equal(kind,"@python"))
                    return fail("this command requires a foreground terminal");
                int status=lx_spawn_flags(path,st.argc,st.args,stream+2*capture);
                if (status<0) { char *msg=query(LAST_ERROR,0,""); return fail(msg ? msg : "process request failed"); }
                s->status=status;
                if (!capture && status && !equal(kind,"@tcc")) printf("[exit %d]\n",status);
            }
        }
        if (st.out && service(STORE_STREAM,st.append,st.out)<0) return -1;
        start=end+1;
    }
    return 0;
}

typedef struct { char *name; int directory; } Match;
typedef struct { Match *data; Size count, cap; } Matches;
static void add_match(Matches *m,const char *name,int directory) {
    if (m->count==m->cap) {
        m->cap=m->cap ? 2*m->cap : 16;
        m->data=checked(realloc(m->data,m->cap*sizeof(Match)));
    }
    m->data[m->count].name=copy(name);
    m->data[m->count++].directory=directory;
}
static int by_name(const void *a,const void *b) {
    return strcmp(((const Match *)a)->name,((const Match *)b)->name);
}
static void scan_dir(Matches *m,const char *dir,const char *leaf,int command,int dirs_only,int explicit_path) {
    char after[LINE+1]="";
    Size d=strlen(dir)+1, p=strlen(leaf)+1;
    if (d>LINE+1 || p>LINE+1) return;
    for (;;) {
        memcpy(MAIL,dir,d); memcpy(MAIL+d,leaf,p); strcpy(MAIL+d+p,after);
        int n=lx_process_call(READ_DIRECTORY,0);
        if (n<=1 || n>LINE+1) break;
        MAIL[n]=0;
        int kind=MAIL[0]-'0';
        strcpy(after,MAIL+1);
        if (dirs_only && kind!=1) continue;
        if (command && ((!explicit_path && kind!=2) || (explicit_path && !kind))) continue;
        if ((!command || explicit_path) && after[0]=='.' && leaf[0]!='.') continue;
        add_match(m,after,kind==1);
    }
}
static void completion(Session *s,int list) {
    Tokens t={0};
    Matches m={0};
    int quote=0;
    if (lex(s,s->line,&t,1,&quote)<0) goto done;
    char *word=t.items[t.count-1].text, *slash=strrchr(word,'/');
    const char *leaf=slash ? slash+1 : word;
    int begin=t.count-1;
    while (begin && t.items[begin-1].kind!=PIPE) begin--;
    int command=begin==t.count-1;
    if (command && !slash) {
        if (prefix("shell",leaf)) add_match(&m,"shell",0);
        const char *builtins[]={"command","history","type","unset","exit"};
        for (unsigned i=0;i<sizeof(builtins)/sizeof(*builtins);++i)
            if (prefix(builtins[i],leaf)) add_match(&m,builtins[i],0);
        const char *part=value(s,"PATH");
        do { char dir[LINE+1]; part=path_dir(part,dir); if (*dir) scan_dir(&m,dir,leaf,1,0,0); } while(part);
    } else {
        if (slash) *slash=0;
        int dirs_only=!command && t.items[begin].kind==WORD && equal(t.items[begin].text,"cd") && t.items[t.count-2].kind==WORD;
        scan_dir(&m,slash ? (*word ? word : "/") : ".",leaf,command,dirs_only,1);
    }
    if (!m.count) goto done;
    qsort(m.data,m.count,sizeof(Match),by_name);
    Size unique=0;
    for (Size i=0;i<m.count;i++) {
        if (unique && equal(m.data[unique-1].name,m.data[i].name)) free(m.data[i].name);
        else m.data[unique++]=m.data[i];
    }
    m.count=unique;
    char *first=m.data[0].name;
    Size common=strlen(first);
    for (Size i=1;i<m.count;i++) {
        Size j=0; while (j<common && first[j]==m.data[i].name[j]) j++;
        common=j;
    }
    // Keep a shared UTF-8 prefix on a character boundary.
    while (common && ((unsigned char)first[common]&0xc0)==0x80) common--;
    if (m.count>1 && list) {
        putchar('\n');
        for (Size i=0;i<m.count;i++) printf("%s%s  ",m.data[i].name,m.data[i].directory ? "/" : "");
        putchar('\n'); prompt(s); printf("%s",s->line);
    }
    char suffix[4*LINE+4]; int n=0;
    for (Size i=strlen(leaf);i<common;i++) {
        int c=first[i];
        if (quote==39 && c==39) { memcpy(suffix+n,"'\\''",4); n+=4; }
        else {
            if ((!quote && strchr(" \\$\042'|<>;&`#",c)) || (quote=='"' && strchr("\\$`\042",c))) suffix[n++]='\\';
            suffix[n++]=c;
        }
    }
    if (m.count==1) {
        if (m.data[0].directory) suffix[n++]='/';
        else { if (quote) suffix[n++]=quote; suffix[n++]=' '; }
    }
    suffix[n]=0;
    if (strlen(s->line)+n<=LINE) { strcpy(s->line+strlen(s->line),suffix); printf("%s",suffix); }
done:
    for (Size i=0;i<m.count;i++) free(m.data[i].name);
    free(m.data); clear_tokens(&t);
}
static int key(void) {
    int c=lx_key_byte();
    if (c!=27) return c;
    c=lx_timed_byte(100);
    if (c!='[' && c!='O') return 0;
    do { c=lx_timed_byte(100); } while (digit(c));
    return c=='A' ? 16 : c=='B' ? 14 : 0;
}
static int read_key(Session *s) {
    int c=key(), skip=s->skip_lf;
    if (c==256) return -1;
    s->skip_lf=0;
    if (skip && c=='\n') return 0;
    int repeat=s->repeated_tab && c==9;
    s->repeated_tab=c==9;
    Size n=strlen(s->line);
    if (c==3) { s->line[0]=s->draft[0]=0; s->position=s->histories; s->status=130; puts("^C"); prompt(s); }
    else if (c==9) completion(s,repeat);
    else if (c=='\n' || c=='\r') { s->skip_lf=c=='\r'; putchar('\n'); return 1; }
    else if (c==16 || c==14) {
        int next=s->position+(c==16 ? -1 : 1);
        if (next<0 || next>s->histories) return 0;
        if (s->position==s->histories) strcpy(s->draft,s->line);
        s->position=next;
        const char *selected=next==s->histories ? s->draft : s->history[next];
        Size i=0; while (i<n && s->line[i]==selected[i]) i++;
        for (Size j=i;j<n;j++) printf("\b \b");
        printf("%s",selected+i); strcpy(s->line,selected);
    } else if ((c==8 || c==127) && n) {
        do { n--; } while (n && ((unsigned char)s->line[n]&0xc0)==0x80);
        s->line[n]=0; printf("\b \b");
    }
    else if (c>=32 && c<=126 && n<LINE) { s->line[n]=c; s->line[n+1]=0; putchar(c); }
    return 0;
}
static Session *login_session(int id,const char *cwd) {
    Session *s=new_session(id,cwd);
    const char *home="/home/lixin";
    if (lx_stat(home)!=LX_DIR) return s;
    char *dir=query(CHDIR,0,home);
    if (!dir) return s;
    strcpy(s->cwd,dir); assign(s,"HOME",home);
    int fd=lx_open("/home/lixin/.profile");
    if (fd<0) return s;
    char block[256],line[LINE+1];
    int used=0,number=0,n;
    do {
        n=lx_read(fd,block,sizeof(block));
        for (int i=0;i<n || (n==0 && used);i++) {
            int c=n ? block[i] : '\n';
            if (c=='\r') continue;
            if (c!='\n') { if (used<=LINE) { if (used<LINE) line[used]=c; used++; } continue; }
            number++;
            Tokens t={0};
            int ok=used<=LINE;
            if (ok) { line[used]=0; ok=lex(s,line,&t,0,0)>=0; }
            if (ok && t.count) {
                int exp=equal(t.items[0].text ? t.items[0].text : "","export");
                if (!exp && t.count==2 && t.items[0].text && equal(t.items[0].text,"cd") && t.items[1].text) {
                    char *d=query(CHDIR,0,t.items[1].text);
                    if (d) strcpy(s->cwd,d); else ok=0;
                } else {
                    ok=t.count>exp && (exp || t.count==1);
                    for (int j=exp;ok && j<t.count;j++)
                        ok=t.items[j].kind==WORD && strchr(t.items[j].text,'=') && binding(s,t.items[j].text,exp)==0;
                }
            }
            if (!ok) printf(".profile: invalid line %d\n",number);
            clear_tokens(&t); used=0;
        }
    } while (n>0);
    lx_close(fd);
    return s;
}
static Terminal *new_terminal(int id,const char *cwd,int login) {
    char inherited[LINE+1];
    strcpy(inherited,cwd);
    Terminal *t=checked(calloc(1,sizeof(*t)));
    t->id=id; t->next_id=2; t->first=t->active=login_session(1,cwd);
    if (!login) {
        char *dir=query(CHDIR,0,inherited);
        if (dir) strcpy(t->active->cwd,dir);
    }
    return t;
}
static void restore(Session *s) {
    if (!query(CHDIR,0,s->cwd)) { query(CHDIR,0,"/"); strcpy(s->cwd,"/"); }
}
static void select_session(Terminal *t,Session *s) {
    t->active=s; restore(s); printf("[shell %d]\n",s->id);
}
static int manage(Terminal *t,Tokens *tokens) {
    int n=tokens->count;
    Token *a=tokens->items;
    if (n>=2 && a[1].kind==WORD) {
        char *op=a[1].text;
        if (n==2 && equal(op,"new")) {
            if (t->next_id==2147483647) return fail("shell IDs exhausted");
            Session **tail=&t->first;
            while (*tail) tail=&(*tail)->next;
            *tail=login_session(t->next_id++,"/"); select_session(t,*tail); return 0;
        }
        if (n==2 && equal(op,"list")) {
            for (Session *s=t->first;s;s=s->next) printf("%c %d  %s\n",s==t->active?'*':' ',s->id,s->cwd);
            return 0;
        }
        if (n==3 && equal(op,"use") && a[2].kind==WORD) {
            unsigned long id=0;
            const char *p=a[2].text;
            while (digit(*p) && id<=2147483647) id=id*10+(*p++-'0');
            if (!*p) for (Session *s=t->first;s;s=s->next) if (s->id==id) { select_session(t,s); return 0; }
            return fail("unknown shell ID");
        }
        if (n==2 && equal(op,"close")) return 1;
    }
    return fail("usage: shell new | shell list | shell use ID | shell close");
}
static int command(Terminal *t) {
    Tokens tokens={0};
    Session *s=t->active;
    int result=lex(s,s->line,&tokens,0,0);
    if ((result<0 || !skip_history(s,&tokens)) &&
        (!s->histories || !equal(s->history[s->histories-1],s->line))) {
        if (request(HISTORY_APPEND,0,s->line)<0) puts("could not save history");
        remember(s,s->line);
    }
    s->position=s->histories; s->line[0]=s->draft[0]=0; s->repeated_tab=0;
    if (result>=0) {
        if (tokens.count && tokens.items[0].kind==WORD && equal(tokens.items[0].text,"shell")) {
            s->status=0; result=manage(t,&tokens);
        } else result=execute(s,&tokens);
    }
    clear_tokens(&tokens);
    if (result<0) { s->status=1; printf("shell: %s\n",error); }
    return result;
}
int main(int argc,char **argv) {
    if (argc>=3 && equal(argv[1],"--stage-builtin")) {
        if (equal(argv[2],"help")) {printf("%s",help);return 0;}
        if (equal(argv[2],"cd")) return argc>4 || !query(CHDIR,0,argc==4 ? argv[3] : "/");
        return argc!=3 || !strchr(argv[2],'=');
    }
    if (argc==2 && equal(argv[1],"--version")) {
        puts(shell_identity);
        return 0;
    }
    if (argc==3 && equal(argv[1],"-c")) {
        if (strlen(argv[2])>LINE) {
            lx_error("Bash command exceeds 512 bytes\n");
            return 2;
        }
        char *cwd=query(CHDIR,0,".");
        Session *s=new_session(1,cwd ? cwd : "/");
        Tokens tokens={0};
        int result=lex(s,argv[2],&tokens,0,0);
        if (result>=0 && tokens.count && tokens.items[tokens.count-1].kind==BACKGROUND)
            result=fail("background commands require an interactive shell");
        if (result>=0) result=execute(s,&tokens);
        clear_tokens(&tokens);
        int status=s->status;
        drop_session(s);
        if (result<0) { printf("shell: %s\n",error); return 1; }
        return status;
    }
    if (argc!=1 || !lx_stdout_is_terminal()) {
        lx_error("Bash requires an interactive terminal\n");
        return 1;
    }
    unsigned flags=lx_start_flags();
    char *cwd=query(CHDIR,0,".");
    lx_console_call(57,0); /* The interactive line editor handles its own Ctrl+C. */
    Terminal *terminals=new_terminal(1,cwd ? cwd : "/",flags&2), *active=terminals;
    Session *initial=active->active;
    for (int i=0;i<64;i++) {
        char *line=query(HISTORY_GET,i,"");
        if (!line) break;
        remember(initial,line);
    }
    initial->position=initial->histories;
    unsigned long screen=lx_geometry();
    if ((flags&2) && (screen>>32) && (screen&65535)>=60) {
        printf("\033[2J\033[H\033[36m  L I X I N O S   SYSTEM CONSOLE\033[0m\n");
        long length = lx_console_call(29, 2); /* uname -r */
        if (length > 0 && length < 128) {
            char release[128];
            memcpy(release, (const char *)0x443d7000, length);
            release[length] = 0;
            printf("  Kernel %s\n", release);
        }
        puts("  ARM64 / QEMU virt   Disk mounted\n  help  Commands     htop  System monitor     shutdown  Power off\n");
    }
    if (flags&2) puts(shell_identity);
    prompt(initial);
    for (;;) {
        if (flags&4) {
            unsigned event=request(NEXT_TERMINAL,0,""), id=event&0x3fffffff;
            Terminal **ptr=&terminals;
            while (*ptr && (*ptr)->id!=id) ptr=&(*ptr)->next;
            if (!*ptr && (event&(1u<<30))) {
                *ptr=new_terminal(id,"/",1);
                puts(shell_identity); prompt((*ptr)->active);
            }
            if (*ptr!=active) { active=*ptr; if (active) restore(active->active); }
            if (event&(1u<<30)) continue;
            if (!active) { lx_key_byte(); continue; }
        }
        int entered=read_key(active->active);
        if (entered<0) return active->active->status;
        if (!entered) continue;
        if (command(active)==1) {
            Session *old=active->active, **ptr=&active->first;
            while (*ptr!=old) ptr=&(*ptr)->next;
            *ptr=old->next;
            int status=old->status; drop_session(old);
            if (active->first) select_session(active,active->first);
            else if (flags&4) {
                Terminal **tp=&terminals;
                while (*tp!=active) tp=&(*tp)->next;
                *tp=active->next; free(active); active=0;
                request(CLOSE_TERMINAL,0,""); continue;
            } else return status;
        }
        prompt(active->active);
    }
}
