#include "links.h"
#include "page.h"
#include "cookies.h"
#include "duktape.h"
#include "../console.h"
#include <strings.h>

int decode_picture(struct page_media *,const struct page_source *,int,int);
/* Allow layout markers and script output some space beyond the input limit. */
#define EXPANDED_LIMIT (PAGE_LIMIT + 2UL*1024*1024)
struct buffer { char *data; size_t length, capacity; };
struct document {
    char url[ADDRESS_LIMIT];
    int dirty,width,dom;
    size_t fed;
    struct buffer out;
    struct picture *previous;
    int previous_count;
    struct page_media *media;
    duk_context *js;
    unsigned deadline, budget, requests;
    size_t heap_bytes;
};
static int append(struct buffer *b,const char *s,size_t n) {
    if (n>EXPANDED_LIMIT-b->length) return 0;
    size_t need=b->length+n+1;
    if (need>b->capacity) {
        size_t capacity=b->capacity?2*b->capacity:4096;
        if (capacity<need) capacity=need;
        if (capacity>EXPANDED_LIMIT+1) capacity=EXPANDED_LIMIT+1;
        char *p=realloc(b->data,capacity);
        if (!p) return 0;
        b->data=p; b->capacity=capacity;
    }
    memcpy(b->data+b->length,s,n); b->length+=n; b->data[b->length]=0;
    return 1;
}
struct element { unsigned char *name,*attrs,*end; int len; };
static int element(char *p,char *end,struct element *e) {
    return !parse_element((unsigned char *)p,(unsigned char *)end,&e->name,&e->len,&e->attrs,&e->end);
}
static int is(const struct element *e,const char *s) {
    return strlen(s)==(size_t)e->len && !strncasecmp((char *)e->name,s,e->len);
}
static char *attribute(const struct element *e,const char *name) {
    struct document_options opts={0}, *saved=d_opt;
    opts.cp=opts.real_cp=utf8_table; d_opt=&opts;
    char *value=(char *)get_attr_val(e->attrs,(unsigned char *)name);
    d_opt=saved;
    return value;
}
static int embedded(struct document *d,const char *ref,struct page_source *src) {
    if (!ref || !*ref || strlen(ref)>=ADDRESS_LIMIT || d->requests>=48) return 0;
    char *url=(char *)join_urls((unsigned char *)d->url,(unsigned char *)ref);
    if (!url) return 0;
    char address[ADDRESS_LIMIT];
    int allowed=strlen(url)<sizeof(address) &&
        (!strncmp(url,"https://",8) ||
         (!strncmp(url,"http://",7) && strncmp(d->url,"https://",8)) ||
         (!strncmp(url,"file://",7) && !strncmp(d->url,"file://",7)));
    if (allowed) strcpy(address,url);
    mem_free(url);
    if (!allowed) return 0;
    ++d->requests;
    return !load_resource(address,d->url,src);
}
/* Raw-text elements and comments must never turn inert markup into script. */
static char *raw_end(struct element *e,char *end,char **closing) {
    char *p=(char *)e->end;
    while ((p=memchr(p,'<',end-p))) {
        struct element tag;
        if (p+1<end && p[1]=='/' && element(p,end,&tag) && tag.len==e->len+1 &&
            !strncasecmp((char *)tag.name+1,(char *)e->name,e->len)) {
            *closing=p; return (char *)tag.end;
        }
        p++;
    }
    *closing=end; return end;
}
static int inert(struct element *e) {
    return is(e,"style")||is(e,"textarea")||is(e,"title")||is(e,"noscript")||
        is(e,"template")||is(e,"iframe")||is(e,"xmp");
}

int script_stack_low(void) {
    extern char __user_heap_start[];
    uintptr_t sp;
    __asm__ volatile("mov %0, sp":"=r"(sp));
    return sp < (uintptr_t)__user_heap_start+32768;
}
double script_time(void) { return (double)console_call(36,0)*1000.; }
int script_timeout(void *ud) {
    struct document *d=ud;
    if (!d->budget) return 1;
    --d->budget;
    return (unsigned long)console_call(36,0)>=d->deadline;
}
/* Keep untrusted script allocations separate from document and image budgets. */
static void *js_realloc(void *ud,void *p,duk_size_t n) {
    struct document *d=ud;
    size_t *old=p?(size_t *)p-1:NULL;
    size_t used=d->heap_bytes-(old?*old:0);
    if (!n) {free(old); d->heap_bytes=used; return NULL;}
    if (n>24UL*1024*1024-used) return NULL;
    size_t *next=realloc(old,sizeof(*next)+n);
    if (!next) return NULL;
    *next=n; d->heap_bytes=used+n; return next+1;
}
static void *js_alloc(void *ud,duk_size_t n) { return js_realloc(ud,NULL,n); }
static void js_free(void *ud,void *p) { js_realloc(ud,p,0); }
static struct document *document(duk_context *ctx) {
    duk_memory_functions m; duk_get_memory_functions(ctx,&m); return m.udata;
}
static duk_ret_t js_title(duk_context *ctx) {
    struct page_media *m=document(ctx)->media;
    if (!duk_get_top(ctx)) { duk_push_string(ctx,m->title?m->title:""); return 1; }
    size_t n; const char *s=duk_to_lstring(ctx,0,&n);
    if (n>4096) return DUK_RET_RANGE_ERROR;
    char *title=strdup(s);
    if (!title) return DUK_RET_ERROR;
    document(ctx)->dirty=1; free(m->title); m->title=title;
    return 0;
}
static void js_fatal(void *ud,const char *msg) { fatal_exit("JavaScript engine stopped"); }
static duk_ret_t js_cookie(duk_context *ctx) {
    struct document *d=document(ctx);
    if (!duk_get_top(ctx)) {
        char text[4096];cookie_list(d->url,text,sizeof(text),0);
        duk_push_string(ctx,text);return 1;
    }
    duk_size_t n;
    const char *value=duk_require_lstring(ctx,0,&n);
    cookie_store(d->url,value,n,0);return 0;
}
static duk_ret_t js_location(duk_context *ctx) {
    struct document *d=document(ctx);
    if (!duk_get_top(ctx)) { duk_push_string(ctx,d->url); return 1; }
    const char *ref=duk_require_string(ctx,0);
    if (strlen(ref)>=ADDRESS_LIMIT) return DUK_RET_RANGE_ERROR;
    char *url=(char *)join_urls((unsigned char *)d->url,(unsigned char *)ref);
    if (!url) return DUK_RET_URI_ERROR;
    int allowed=(strlen(url)<ADDRESS_LIMIT && (!strncmp(url,"http://",7) || !strncmp(url,"https://",8) ||
        (!strncmp(url,"file://",7) && !strncmp(d->url,"file://",7))));
    if (allowed) {
        strcpy(d->media->navigate,url);
        d->media->replace=duk_get_boolean(ctx,1);
    }
    mem_free(url);
    return allowed?0:DUK_RET_URI_ERROR;
}
static duk_ret_t js_now(duk_context *ctx) {
    duk_push_number(ctx,(double)console_call(38,0)); return 1;
}
static duk_ret_t push_response(duk_context *ctx,void *opaque) {
    struct page_source *response=opaque;
    duk_push_object(ctx);
    duk_push_int(ctx,response->status); duk_put_prop_string(ctx,-2,"status");
    duk_push_lstring(ctx,response->body,response->length); duk_put_prop_string(ctx,-2,"body");
    return 1;
}
static duk_ret_t js_fetch(duk_context *ctx) {
    struct document *d=document(ctx);
    const char *url=duk_require_string(ctx,0);
    struct page_source response={0};
    if (d->requests++>=128) return duk_error(ctx,DUK_ERR_RANGE_ERROR,"Page request limit");
    const char *error=fetch_same_origin(d->url,url,&response);
    if (error) return duk_error(ctx,DUK_ERR_TYPE_ERROR,"%s",error);
    if (response.length>1024*1024) { free(response.bytes); return DUK_RET_RANGE_ERROR; }
    int status=duk_safe_call(ctx,push_response,&response,0,1);
    free(response.bytes);
    if (status!=DUK_EXEC_SUCCESS) return duk_throw(ctx);
    return 1;
}
static void report_error(struct document *d);
static duk_ret_t js_dirty(duk_context *ctx) {document(ctx)->dirty=1; return 0;}
static int start_js(struct document *d) {
    d->js=duk_create_heap(js_alloc,js_realloc,js_free,d,js_fatal);
    if (!d->js) return 0;
    duk_context *ctx=d->js;
    duk_push_c_function(ctx,js_title,DUK_VARARGS); duk_put_global_string(ctx,"__title");
    duk_push_c_function(ctx,js_cookie,DUK_VARARGS); duk_put_global_string(ctx,"__cookie");
    duk_push_c_function(ctx,js_location,DUK_VARARGS); duk_put_global_string(ctx,"__location");
    duk_push_c_function(ctx,js_now,0); duk_put_global_string(ctx,"__now");
    duk_push_c_function(ctx,js_fetch,1); duk_put_global_string(ctx,"__fetch");
    duk_push_c_function(ctx,js_dirty,0); duk_put_global_string(ctx,"__dirty");
    static const char events[]=
#include "events.inc"
    ;
    int ok=!duk_peval_lstring(ctx,events,sizeof(events)-1);
    duk_pop(ctx);
    if (ok) {
        char path[ADDRESS_LIMIT]="file:///src/links/dom.js";
        struct page_source source={0};
        duk_get_global_string(ctx,"__bindDOM");
        const char *error=load_page(path,&source);
        if (error) duk_push_string(ctx,error);
        ok=!error && !duk_peval_lstring(ctx,source.body,source.length) && !duk_pcall(ctx,1);
        if (!ok) report_error(d);
        free(source.bytes);
        duk_set_top(ctx,0);
    }
    if (ok) {
        d->dom=1;
        duk_push_heap_stash(ctx);
        const char *names[]={"__tick","__click","__ready","__feed","__serialize","__endScript"};
        for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
            duk_get_global_string(ctx,names[i]);duk_put_prop_string(ctx,-2,names[i]);
        }
        duk_pop(ctx);
    }
    return ok;
}
static void callback(duk_context *ctx,const char *name) {
    duk_push_heap_stash(ctx);duk_get_prop_string(ctx,-1,name);duk_remove(ctx,-2);
}
static void report_error(struct document *d) {
    struct page_media *m=d->media;
    if (!m->script_errors) snprintf(m->first_error,sizeof(m->first_error),"Script: %.115s",duk_safe_to_string(d->js,-1));
    m->script_errors++;
}
static duk_ret_t feed_markup(duk_context *ctx,void *arg) {
    struct document *d=document(ctx);
    callback(ctx,"__feed");
    duk_push_lstring(ctx,d->out.data?d->out.data+d->fed:"",d->out.length-d->fed);
    duk_push_boolean(ctx,*(int *)arg);
    duk_call(ctx,2);
    return 0;
}
static void feed(struct document *d,int eof) {
    if (!d->dom) return;
    if (duk_safe_call(d->js,feed_markup,&eof,0,1)!=DUK_EXEC_SUCCESS) {
        report_error(d);
        d->dom=0;
    }
    duk_pop(d->js);
    d->fed=d->out.length;
}
static void script(struct document *d,struct element *e,char *closing) {
    char *type=attribute(e,"type");
    int classic=!type || !*type || !strcasecmp(type,"text/javascript") || !strcasecmp(type,"application/javascript");
    if (type) mem_free(type);
    if (!classic) return;
    char *ref=attribute(e,"src");
    struct page_source src={0};
    char *code=(char *)e->end;
    size_t n=closing-code;
    if (ref) {
        int ok=embedded(d,ref,&src); mem_free(ref);
        if (!ok) { d->media->script_errors++; return; }
        code=src.body; n=src.length;
    }
    d->deadline=(unsigned)console_call(36,0)+3;
    if (d->budget && (d->dom || (!d->js && start_js(d)))) {
        feed(d,0);
        if (duk_peval_lstring(d->js,code,n)) report_error(d);
        duk_pop(d->js);
        callback(d->js,"__endScript"); if (duk_pcall(d->js,0)) report_error(d);
        duk_pop(d->js);
    } else d->media->script_errors++;
    free(src.bytes);
}
static int picture(struct document *d,struct element *e,int width) {
    int index=d->media->count;
    if (index>=PICTURE_COUNT) return 0;
    char *ref=attribute(e,"src");
    if (!ref) return 0;
    char *size=attribute(e,"width");
    int wanted=size?atoi(size):0;
    if (size) mem_free(size);
    struct picture *im=&d->media->images[index];
    int cached=0;
    for (int i=0;i<d->previous_count;i++) {
        struct picture *old=&d->previous[i];
        if (old->pixels && (size_t)old->width*old->height<=4194304-d->media->pixels && old->requested_width==wanted && !strcmp(old->source,ref)) {
            *im=*old; old->source=NULL;old->pixels=NULL;
            d->media->count++; d->media->pixels+=(size_t)im->width*im->height;
            cached=1;break;
        }
    }
    if (!cached) {
        struct page_source src={0};
        int ok=embedded(d,ref,&src) && decode_picture(d->media,&src,width,wanted);
        free(src.bytes);
        if (!ok) {mem_free(ref);return 0;}
        im->requested_width=wanted;
        im->source=ref;
    } else mem_free(ref);
    int rows=(d->media->images[index].shown_height+WEB_LINE-1)/WEB_LINE;
    if (!append(&d->out,"<br>",4)) return -1;
    for (int row=0;row<rows;row++) {
        char buf[32];
        int len=snprintf(buf,sizeof(buf),"&#%u;<br>",PICTURE_MARK+index*PICTURE_ROWS+row);
        if (!append(&d->out,buf,len)) return -1;
    }
    return 1;
}
static int transform(struct document *d,char *p,char *end,int images,int width) {
    char *copied=p;
    while ((p=memchr(p,'<',end-p))) {
        if (end-p>=4 && !memcmp(p,"<!--",4)) {
            char *close=strstr(p+4,"-->");
            p=close?close+3:end; continue;
        }
        struct element e;
        if (!element(p,end,&e)) {p++; continue;}
        char *next=(char *)e.end, *closing;
        if (is(&e,"script") || inert(&e)) next=raw_end(&e,end,&closing);
        if (!images && is(&e,"title") && !d->media->title && closing-(char *)e.end<=4096) {
            struct document_options opts={0}; opts.cp=utf8_table;
            unsigned char *title=convert_string(NULL,e.end,closing-(char *)e.end,&opts);
            d->media->title=strdup((char *)title);
            mem_free(title);
        }
        if ((!images && is(&e,"script")) || (images && is(&e,"img"))) {
            if (!append(&d->out,copied,p-copied)) return 0;
            if (!images) {
                if (!append(&d->out,p,next-p)) return 0;
                script(d,&e,closing);
            }
            else {
                int result=picture(d,&e,width);
                if (result<0 || (!result && !append(&d->out,p,next-p))) return 0;
            }
            copied=next;
        }
        p=next;
    }
    return append(&d->out,copied,end-copied);
}
const char *prepare_page(struct page_source *src,const char *url,int width,struct page_media *media) {
    struct document *d=calloc(1,sizeof(*d));
    if (!d) return "Not enough browser memory";
    strcpy(d->url,url); d->media=media; d->budget=4096; d->width=width;
    media->runtime=d;
    if (src->headers && !(media->headers=strdup(src->headers))) return "Not enough browser memory";
    if (!transform(d,src->body,src->body+src->length,0,width)) return "Expanded page exceeds renderer memory limit";
    if (d->dom) {
        feed(d,1);
        d->deadline=(unsigned)console_call(36,0)+3;
        if (d->dom && d->budget) { callback(d->js,"__ready"); if (duk_pcall(d->js,0)) report_error(d); }
        if (duk_get_top(d->js)) duk_pop(d->js);
    }
    if (!page_snapshot(media)) return "Expanded page exceeds renderer memory limit";
    src->body=media->html; src->length=strlen(media->html);
    return NULL;
}
int page_snapshot(struct page_media *m) {
    struct document *d=m->runtime; d->media=m;
    if (d->dom) {
        callback(d->js,"__serialize");
        if (duk_pcall(d->js,0)) report_error(d);
        else {
            duk_size_t n;
            const char *html=duk_get_lstring(d->js,-1,&n);
            struct buffer serialized={0};
            if (html && append(&serialized,html,n)) {
                free(d->out.data); d->out=serialized; d->fed=serialized.length;
            }
        }
        duk_pop(d->js);
    }
    struct buffer source=d->out;
    d->out=(struct buffer){0};
    struct picture previous[PICTURE_COUNT];
    memcpy(previous,m->images,m->count*sizeof(*previous));
    d->previous=previous;d->previous_count=m->count;
    m->count=0; m->pixels=0;
    int ok=d->width ? transform(d,source.data,source.data+source.length,1,d->width)
                    : append(&d->out,source.data,source.length);
    for (int i=0;i<d->previous_count;i++) {
        free(previous[i].pixels); if (previous[i].source) mem_free(previous[i].source);
    }
    d->previous=NULL; d->previous_count=0;
    if (ok) { free(m->html); m->html=d->out.data; }
    else free(d->out.data);
    d->out=source; d->dirty=0;
    return ok;
}
static void turn(struct document *d) {
    d->budget=128;
    d->deadline=(unsigned)console_call(36,0)+3;
}
int page_tick(struct page_media *m) {
    struct document *d=m->runtime;
    if (!d || !d->dom) return 0;
    d->media=m; turn(d);
    callback(d->js,"__tick");
    if (duk_pcall(d->js,0)) report_error(d);
    duk_pop(d->js);
    return d->dirty;
}
/* Links identifies controls by the byte offset of their attributes. Map
   anonymous controls back to the same tag in DOM order after layout. */
static int control_index(char *html,int offset,char tag[16]) {
    char *end=html+strlen(html), *p=html;
    struct element e;
    int buttons=0,inputs=0,textareas=0;
    while ((p=memchr(p,'<',end-p)) && p<=html+offset) {
        if (end-p>=4 && !memcmp(p,"<!--",4)) {
            char *close=strstr(p+4,"-->"); p=close?close+3:end; continue;
        }
        if (!element(p,end,&e)) {p++;continue;}
        int *index=is(&e,"button")?&buttons:is(&e,"input")?&inputs:is(&e,"textarea")?&textareas:NULL;
        if (index) {
            if ((char *)e.attrs==html+offset) {
                memcpy(tag,e.name,e.len); tag[e.len]=0; return *index;
            }
            ++*index;
        }
        p=(char *)e.end;
        if (inert(&e) || is(&e,"script")) {char *closing;p=raw_end(&e,end,&closing);}
    }
    return -1;
}
int page_click(struct page_media *m,int offset) {
    struct document *d=m->runtime;
    if (offset<0 || !m->html || (size_t)offset>=strlen(m->html)) return 0;
    struct element e={.attrs=(unsigned char *)m->html+offset};
    char *id=attribute(&e,"id"), *inline_code=attribute(&e,"onclick");
    d->media=m; turn(d);
    int handled=d->dom || (inline_code && !d->js && start_js(d));
    if (handled && !d->fed) feed(d,1);
    if (handled) {
        callback(d->js,"__click");
        char tag[16]="";
        int index=id && *id?-1:control_index(m->html,offset,tag);
        duk_push_string(d->js,id?id:""); duk_push_string(d->js,tag); duk_push_int(d->js,index);
        if (duk_pcall(d->js,3)) {m->script_errors++; handled=0;}
        else handled=duk_to_boolean(d->js,-1);
        duk_pop(d->js);
    }
    if (id) mem_free(id);
    if (inline_code) mem_free(inline_code);
    return handled;
}
void release_page(struct page_media *m) {
    if (m->runtime) {
        m->runtime->media=m;
        if (m->runtime->js) duk_destroy_heap(m->runtime->js);
        free(m->runtime->out.data); free(m->runtime);
    }
    free(m->html); free(m->title); free(m->headers);
    for (int i=0;i<m->count;i++) {free(m->images[i].pixels);mem_free(m->images[i].source);}
}
