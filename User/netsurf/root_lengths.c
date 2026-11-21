// Expand bounded, same-sheet custom property references for the framebuffer port.
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct slice { const char *p, *end; };
struct definition { struct slice name, value, media, scope; size_t next; };
struct lengths {
    struct definition *defs; size_t used, capacity;
    char *out; size_t length, room, limit;
    size_t first[256], last[256], steps;
    bool writing, failed;
};
static struct slice trim(struct slice s) {
    while (s.p<s.end && isspace((unsigned char)*s.p)) s.p++;
    while (s.end>s.p && isspace((unsigned char)s.end[-1])) s.end--;
    return s;
}
static bool equal(struct slice a, struct slice b) {
    return a.end-a.p==b.end-b.p && !memcmp(a.p,b.p,a.end-a.p);
}
static bool is(struct slice a,const char *text) {
    return equal(a,(struct slice){text,text+strlen(text)});
}
static void append(struct lengths *c,const char *s,size_t n) {
    if (c->failed || n>c->limit-c->length) { c->failed=true; return; }
    if (c->length+n>=c->room) {
        size_t cap=(c->length+n)*2+128;
        char *out=realloc(c->out,cap);
        if (!out) { c->failed=true; return; }
        c->out=out; c->room=cap;
    }
    memcpy(c->out+c->length,s,n); c->length+=n;
}
static void put(struct lengths *c,struct slice s) { append(c,s.p,s.end-s.p); }

// Find delimiters outside strings, comments, escaped characters and functions.
static const char *scan(const char *p,const char *end,const char *delims) {
    char quote=0; int paren=0;
    while (p<end) {
        char ch=*p;
        if (ch=='\\') { p+=end-p>=2?2:1; continue; }
        if (quote) { if (ch==quote) quote=0; p++; continue; }
        if (ch=='"' || ch=='\'') { quote=ch; p++; continue; }
        if (ch=='/' && end-p>=2 && p[1]=='*') {
            p+=2; while (end-p>=2 && (p[0]!='*'||p[1]!='/')) p++;
            if (end-p<2) return end;
            p+=2; continue;
        }
        if (!paren && strchr(delims,ch)) break;
        if (ch=='(') paren++; else if (paren && ch==')') paren--;
        p++;
    }
    return p;
}
static const char *closing(const char *p,const char *end) {
    int depth=1;
    while ((p=scan(p,end,"{}"))<end) {
        if (*p=='{') depth++; else if (!--depth) break;
        p++;
    }
    return p;
}
static bool length_literal(struct slice v) {
    const char *p=v.p;
    if (p==v.end || (!isdigit((unsigned char)*p) && *p!='.')) return false;
    while (p<v.end && (isdigit((unsigned char)*p)||*p=='.')) ++p;
    struct slice unit={p,v.end};
    return is(unit,"px")||is(unit,"rem")||is(unit,"em")||is(unit,"vh")||is(unit,"vw")||is(unit,"%")||is(v,"0");
}
static struct slice variable(struct slice v) {
    if (v.end-v.p<8 || memcmp(v.p,"var(",4) || v.end[-1]!=')') return (struct slice){0};
    struct slice key=trim((struct slice){v.p+4,v.end-1});
    return key.end-key.p>2 && !memcmp(key.p,"--",2) ? key : (struct slice){0};
}
static bool simple_scope(struct slice s) {
    if (s.end-s.p<2 || (*s.p!='#' && *s.p!='.')) return false;
    for (const char *p=s.p+1;p<s.end;p++)
        if (!isalnum((unsigned char)*p) && *p!='_' && *p!='-') return false;
    return true;
}
static bool literal(struct slice v) {
    if (length_literal(v) || is(v,"transparent") || is(v,"currentColor") ||
        is(v,"block") || is(v,"inline-block") || is(v,"inline") ||
        is(v,"solid") || is(v,"none")) return true;
    size_t n=v.end-v.p;
    if (n>5 && v.end[-1]==')' &&
        (!strncasecmp(v.p,"url(",4) || !strncasecmp(v.p,"rgb(",4) ||
         !strncasecmp(v.p,"rgba(",5))) return true;
    if (n<2 || *v.p!='#' || !(n==4 || n==5 || n==7 || n==9)) return false;
    for (const char *p=v.p+1;p<v.end;p++) if (!isxdigit((unsigned char)*p)) return false;
    return true;
}
static unsigned bucket(struct slice key) {
    unsigned h=5381;
    for (const char *p=key.p;p<key.end;p++) h=h*33+(unsigned char)*p;
    return h&255;
}
static void remember(struct lengths *c,struct slice key,struct slice value,
                     struct slice media,struct slice scope) {
    if (c->used==c->capacity) {
        size_t cap=c->capacity*2+16;
        if (cap>8192) { c->failed=true; return; }
        void *d=realloc(c->defs,cap*sizeof(*c->defs));
        if (!d) { c->failed=true; return; }
        c->defs=d; c->capacity=cap;
    }
    size_t i=c->used++;
    c->defs[i]=(struct definition){key,value,media,scope,0};
    unsigned h=bucket(key);
    if (c->last[h]) c->defs[c->last[h]-1].next=i+1;
    else c->first[h]=i+1;
    c->last[h]=i+1;
}

static void guard(struct lengths *c,struct slice scope) {
    append(c,":where(",7); put(c,scope); append(c,")",1);
}
static void selectors(struct lengths *c,struct slice list,struct slice scope) {
    if (!scope.p) { put(c,list); return; }
    for (const char *p=list.p;p<list.end;) {
        const char *end=scan(p,list.end,",");
        struct slice s=trim((struct slice){p,end});
        guard(c,scope); append(c," ",1); put(c,s); append(c,",",1);
        /* The defining element can itself be the subject of the rule. */
        const char *pseudo=scan(s.p,s.end,":");
        while (pseudo<s.end && (s.end-pseudo<2 || pseudo[1]!=':'))
            pseudo=scan(pseudo+1,s.end,":");
        append(c,s.p,pseudo-s.p); guard(c,scope); append(c,pseudo,s.end-pseudo);
        if (end<list.end) append(c,",",1);
        p=end+(end<list.end);
    }
}
/* Fold same-unit padding differences when the subtracted variable has one
 * unambiguous value everywhere in this sheet and applies to the selector. */
static struct slice padding_difference(struct lengths *c, struct slice selector,
        struct slice value, struct slice *subtract)
{
    struct slice none={0};
    if (value.end-value.p<8 || memcmp(value.p,"calc(",5) || value.end[-1]!=')') return none;
    struct slice expr=trim((struct slice){value.p+5,value.end-1});
    const char *split=scan(expr.p,expr.end," \r\n\t\f");
    struct slice left=variable((struct slice){expr.p,split});
    struct slice rest=trim((struct slice){split,expr.end});
    if (!left.p || rest.end-rest.p<2 || *rest.p!='-' || !isspace((unsigned char)rest.p[1])) return none;
    struct slice right=variable(trim((struct slice){rest.p+1,rest.end}));
    if (!right.p) return none;
    struct slice constant={0};
    bool applies=false;
    for (size_t i=c->first[bucket(right)];i;i=c->defs[i-1].next) {
        const struct definition *d=&c->defs[i-1];
        if (!equal(d->name,right)) continue;
        if (!length_literal(d->value) || (constant.p && !equal(constant,d->value))) return none;
        constant=d->value;
        if (!d->media.p && (!d->scope.p || equal(d->scope,selector))) applies=true;
    }
    if (!applies) return none;
    *subtract=constant;
    return left;
}
struct use { struct slice selector,property,important,subtract; };
static void expand(struct lengths *c,const struct use *use,struct slice key,
                   struct slice scope,const struct definition **path,unsigned depth) {
    if (depth==12) return;
    for (size_t i=c->first[bucket(key)];i && !c->failed;i=c->defs[i-1].next) {
        if (++c->steps>2000000) { c->failed=true; return; }
        const struct definition *d=&c->defs[i-1];
        if (!equal(d->name,key)) continue;
        bool seen=false;
        for (unsigned j=0;j<depth;j++) if (path[j]==d) seen=true;
        if (seen || (scope.p && d->scope.p && !equal(scope,d->scope))) continue;
        struct slice effective=scope.p ? scope : d->scope;
        path[depth]=d;
        struct slice alias=variable(d->value);
        if (alias.p) { expand(c,use,alias,effective,path,depth+1); continue; }
        char folded[64];
        struct slice result=d->value;
        if (use->subtract.p) {
            if (!length_literal(result)) continue;
            char *left_unit, *right_unit;
            double left=strtod(result.p,&left_unit), right=strtod(use->subtract.p,&right_unit);
            struct slice unit={left_unit,result.end};
            if (!equal(unit,(struct slice){right_unit,use->subtract.end}) || left>1e7 || right>1e7) continue;
            int size=snprintf(folded,sizeof(folded),"%.8g%.*s",left>right?left-right:0.,
                              (int)(unit.end-unit.p),unit.p);
            if (size<=0 || (size_t)size>=sizeof(folded)) continue;
            result=(struct slice){folded,folded+size};
        }
        for (unsigned j=0;j<=depth;j++) if (path[j]->media.p) {
            put(c,path[j]->media); append(c,"{",1);
        }
        selectors(c,use->selector,effective); append(c,"{",1); put(c,use->property);
        append(c,":",1); put(c,result);
        if (use->important.p) put(c,use->important);
        append(c,"}",1);
        for (unsigned j=0;j<=depth;j++) if (path[j]->media.p) append(c,"}",1);
    }
}
/* Split at each variable use so later declarations in the same rule keep
 * their cascade order. The original values also stay available to libcss. */
static void rule(struct lengths *c,struct slice selector,struct slice body,struct slice media) {
    const char *copied=body.p;
    for (const char *p=body.p;p<body.end && !c->failed;) {
        const char *end=scan(p,body.end,";"), *colon=scan(p,end,":");
        struct slice name=trim((struct slice){p,colon});
        p=end+(end<body.end);
        if (colon==end || name.p==name.end) continue;
        struct slice value=trim((struct slice){colon+1,end});
        if (name.end-name.p>2 && !memcmp(name.p,"--",2)) {
            if (!c->writing && (is(selector,":root") || simple_scope(selector)) &&
                (literal(value) || variable(value).p))
                remember(c,name,value,media,simple_scope(selector)?selector:(struct slice){0});
        } else if (c->writing) {
            const char *bang=scan(value.p,value.end,"!");
            struct slice important={0};
            if (bang<value.end) {
                important=(struct slice){bang,value.end};
                value=trim((struct slice){value.p,bang});
            }
            struct slice subtract={0}, key=variable(value);
            if (!key.p && (is(name,"padding-inline") || is(name,"padding-block")))
                key=padding_difference(c,selector,value,&subtract);
            if (!key.p || !c->first[bucket(key)]) continue;
            append(c,copied,p-copied); append(c,"}",1);
            const struct definition *path[12];
            struct use use={selector,name,important,subtract};
            expand(c,&use,key,(struct slice){0},path,0);
            put(c,selector); append(c,"{",1); copied=p;
        }
    }
    if (c->writing) append(c,copied,body.end-copied);
}
static void rules(struct lengths *c,struct slice sheet,struct slice media,int depth) {
    const char *p=sheet.p,*copied=p;
    while (p<sheet.end && !c->failed) {
        while (p<sheet.end && isspace((unsigned char)*p)) p++;
        if (sheet.end-p>=2 && p[0]=='/' && p[1]=='*') {
            p+=2; while (sheet.end-p>=2 && (p[0]!='*'||p[1]!='/')) ++p;
            p+=sheet.end-p>=2?2:0; if (sheet.end-p<2) break; continue;
        }
        const char *open=scan(p,sheet.end,"{;");
        if (open==sheet.end) break;
        if (*open==';') { p=open+1; continue; }
        const char *close=closing(open+1,sheet.end);
        if (close==sheet.end) break;
        struct slice sel=trim((struct slice){p,open}), body={open+1,close};
        if (sel.end-sel.p>=6 && !memcmp(sel.p,"@media",6) && depth<4) {
            if (c->writing) append(c,copied,open+1-copied);
            if (c->writing || !depth) rules(c,body,sel,depth+1);
            copied=close;
        } else if (sel.p<sel.end && *sel.p!='@') {
            if (c->writing) append(c,copied,open+1-copied);
            rule(c,sel,body,media);
            copied=close;
        }
        p=close+1;
    }
    if (c->writing) append(c,copied,sheet.end-copied);
}
char *ns_root_lengths(const char *data,size_t size,size_t *result_size) {
    if (size>2*1024*1024 || size<8 || !memmem(data,size,"var(--",6)) return NULL;
    struct lengths c={.limit=size*4+4096};
    struct slice sheet={data,data+size};
    rules(&c,sheet,(struct slice){0},0);
    if (c.used && !c.failed) { c.writing=true; rules(&c,sheet,(struct slice){0},0); }
    free(c.defs);
    if (c.failed || !c.out) { free(c.out); return NULL; }
    *result_size=c.length;
    return c.out;
}
