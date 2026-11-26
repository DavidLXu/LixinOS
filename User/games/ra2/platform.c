// Native framebuffer, input and interpreter host for the Vanguard skirmish.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "../../console.h"
#include "../../gui/lixin-gui.h"

struct texture { char name[25]; unsigned w,h; unsigned char *rgba; };
static struct texture *sprites;
static unsigned count;
static struct lx_canvas canvas;
static struct lx_font font;

static unsigned char *read_file(const char *name, size_t *size) {
    int fd=lx_open(name);
    if (fd<0) return NULL;
    size_t capacity=65536, used=0;
    unsigned char *data=malloc(capacity+1);
    while (data) {
        if (used==capacity) {
            if (capacity>=64*1024*1024) break;
            capacity*=2;
            unsigned char *next=realloc(data,capacity+1);
            if (!next) break;
            data=next;
        }
        int n=lx_read(fd,data+used,capacity-used);
        if (n<0) break;
        if (!n) {data[used]=0;*size=used;lx_close(fd);return data;}
        used+=n;
    }
    free(data);lx_close(fd);return NULL;
}
static int compare_texture(const void *a,const void *b) {
    return strcmp(((const struct texture *)a)->name,((const struct texture *)b)->name);
}
static int load_sprites(void) {
    size_t n;
    unsigned char *data=read_file("/lib/ra2/sprites.bin",&n);
    if (!data || n<8 || memcmp(data,"LRA2",4)) return -1;
    memcpy(&count,data+4,4);
    if (count>65536) return -1;
    sprites=calloc(count,sizeof(*sprites));
    if (!sprites) return -1;
    size_t off=8;
    for (unsigned i=0;i<count;i++) {
        if (n-off<28) return -1;
        struct texture *s=sprites+i;
        memcpy(s->name,data+off,24);
        s->w=data[off+24]+256u*data[off+25]; s->h=data[off+26]+256u*data[off+27];
        off+=28;
        size_t bytes=(size_t)s->w*s->h*4;
        if (bytes>n-off) return -1;
        s->rgba=data+off;off+=bytes;
    }
    qsort(sprites,count,sizeof(*sprites),compare_texture);
    return off==n?0:-1;
}
static void draw_sprite(const struct texture *s,int x,int y) {
    x-=(int)s->w/2;y-=(int)s->h;
    for (int row=0;row<(int)s->h;row++) {
        if ((unsigned)(y+row)>=canvas.height) continue;
        for (int col=0;col<(int)s->w;col++) {
            if ((unsigned)(x+col)>=canvas.width) continue;
            const unsigned char *p=s->rgba+4*(row*s->w+col);
            unsigned a=p[3];
            if (!a) continue;
            unsigned *dst=canvas.pixels+(y+row)*canvas.width+x+col;
            unsigned rgb=p[0]*65536u+p[1]*256u+p[2];
            if (a==255) { *dst=rgb;continue; }
            unsigned d=*dst;
            *dst=(((p[0]*a+((d>>16)&255)*(255-a))/255)<<16) |
                 (((p[1]*a+((d>>8)&255)*(255-a))/255)<<8) |
                 ((p[2]*a+(d&255)*(255-a))/255);
        }
    }
}
enum { RECT, SPRITE, TEXT, LOG };
static JSValue native(JSContext *ctx,JSValueConst self,int argc,JSValueConst *args,int op) {
    int32_t v[5]={0};
    int string_index=op==RECT?-1:op==TEXT?2:0;
    const char *s=NULL;
    for (int i=0;i<argc && i<5;i++) {
        if (i!=string_index && JS_ToInt32(ctx,&v[i],args[i])<0) return JS_EXCEPTION;
    }
    if (string_index>=0) {
        if (string_index>=argc) return JS_UNDEFINED;
        s=JS_ToCString(ctx,args[string_index]);
        if (!s) return JS_EXCEPTION;
    }
    if (op==RECT) {
        if (v[0]<0) {v[2]+=v[0];v[0]=0;}
        if (v[1]<0) {v[3]+=v[1];v[1]=0;}
        if(v[2]>0 && v[3]>0) lx_gui_rect(&canvas,v[0],v[1],v[2],v[3],v[4]);
    } else if (op==SPRITE) {
        struct texture key={0};snprintf(key.name,sizeof(key.name),"%s",s);
        const struct texture *t=bsearch(&key,sprites,count,sizeof(*sprites),compare_texture);
        if(t) draw_sprite(t,v[1],v[2]);
    } else if(op==TEXT) lx_gui_text(&canvas,&font,v[0],v[1],canvas.width/9,s,v[3]);
    else { puts(s);fflush(stdout); }
    if (s) JS_FreeCString(ctx,s);
    return JS_UNDEFINED;
}
static int exception(JSContext *ctx) {
    JSValue error=JS_GetException(ctx);
    JSValue stack=JS_GetPropertyStr(ctx,error,"stack");
    const char *msg=JS_ToCString(ctx,error), *trace=JS_ToCString(ctx,stack);
    fprintf(stderr,"ra2: %s\n%s\n",msg?msg:"error",trace?trace:"");
    JS_FreeCString(ctx,msg);JS_FreeCString(ctx,trace);
    JS_FreeValue(ctx,stack);JS_FreeValue(ctx,error);
    return 1;
}
static int key_event(void) {
    int k=lx_timed_byte(0);
    if(k!=27) return k;
    if(lx_timed_byte(12)!='[') return 27;
    int next=lx_timed_byte(12);
    return next>='A' && next<='D'?301+next-'A':27;
}
int main(int argc,char **argv) {
    int check=argc>1 && !strcmp(argv[1],"--check");
    if (!check && (lx_gui_open(&canvas)<0 || lx_gui_font(&font)<0 || load_sprites()<0)) {
        fprintf(stderr,"ra2: open a graphical terminal and check the installed font and sprites\n"); return 1;
    }
    JSRuntime *rt=JS_NewRuntime();if(!rt) return 1;
    JS_SetMaxStackSize(rt,512*1024);
    JSContext *ctx=JS_NewContext(rt);if(!ctx) return 1;
    JSValue global=JS_GetGlobalObject(ctx);
    const char *names[]={"rect","sprite","text","log"};
    const unsigned lengths[]={5,3,4,1};
    for(int i=0;i<4;i++)
        JS_SetPropertyStr(ctx,global,names[i],JS_NewCFunctionMagic(ctx,native,names[i],lengths[i],JS_CFUNC_generic_magic,i));
    size_t size;
    unsigned char *data=read_file("/lib/ra2/match.json",&size);
    if (!data) {puts("ra2: missing match.json");return 1;}
    JSValue match=JS_ParseJSON(ctx,(char *)data,size,"match.json");free(data);
    if(JS_IsException(match)) return exception(ctx);
    JS_SetPropertyStr(ctx,global,"match",match);
    data=read_file("/lib/ra2/skirmish.js",&size);
    if (!data) {puts("ra2: missing skirmish.js");return 1;}
    JSValue result=JS_Eval(ctx,(char *)data,size,"skirmish.js",JS_EVAL_TYPE_GLOBAL);free(data);
    if(JS_IsException(result)) return exception(ctx);
    JS_FreeValue(ctx,result);
    JSValue fn=JS_GetPropertyStr(ctx,global,check?"check":"frame");
    int status=0;
    if(check) {
        result=JS_Call(ctx,fn,global,0,NULL);
        if(JS_IsException(result)) status=exception(ctx);
        JS_FreeValue(ctx,result);
    } else {
        struct lx_pointer pointer={0};
        unsigned last=console_call(38,0);
        int first=1;
        for(;;) {
            unsigned start=console_call(38,0);
            int key=key_event();if(key=='q'||key=='Q'||key==17||key==3) break;
            lx_gui_pointer(&pointer);
            int inside=pointer.inside && !pointer.cancelled;
            unsigned steps=(start-last)/66;
            last+=steps*66;
            JSValue args[]={JS_NewInt32(ctx,canvas.width),JS_NewInt32(ctx,canvas.height),
                JS_NewInt32(ctx,inside?(int)pointer.x:-1),JS_NewInt32(ctx,inside?(int)pointer.y:-1),
                JS_NewBool(ctx,inside&&pointer.down),JS_NewInt32(ctx,key),JS_NewInt32(ctx,steps>4?4:steps)};
            result=JS_Call(ctx,fn,global,7,args);
            if(JS_IsException(result)) {status=exception(ctx);break;}
            JS_FreeValue(ctx,result);
            if(lx_gui_present(&canvas)<0) break;
            if(inside) lx_gui_cursor_tile(&canvas,pointer.x,pointer.y,1,0);
            if(first) {puts("RA2 skirmish ready");fflush(stdout);first=0;}
            unsigned elapsed=(unsigned)console_call(38,0)-start;
            if(elapsed<50) console_call(43,50-elapsed);
        }
    }
    JS_FreeValue(ctx,fn);JS_FreeValue(ctx,global);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    return status;
}
