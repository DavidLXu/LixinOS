// Included with the outline rasterizer to share its TrueType implementation.
#include <stdbool.h>
#include "shaping.h"
#include "hb.h"
#include "hb-ot.h"

enum face_id { BASE_FACE,ARABIC,HEBREW,DEVANAGARI,TAMIL,FACE_COUNT };
struct font_face { int tried,units; hb_font_t *hb; stbtt_fontinfo tt; };
static struct font_face faces[FACE_COUNT];
static int script_face(unsigned c) {
    if ((c>=0x370 && c<0x530) || (c>=0x1f00 && c<0x2000)) return BASE_FACE;
    if ((c>=0x600 && c<0x700) || (c>=0x750 && c<=0x77f) || (c>=0x8a0 && c<0x900)) return ARABIC;
    if (c>=0x590 && c<0x600) return HEBREW;
    if (c>=0x900 && c<0x980) return DEVANAGARI;
    if (c>=0xb80 && c<0xc00) return TAMIL;
    return -1;
}
static struct font_face *shape_face(int id) {
    static const char *names[]={"NotoSans","NotoSansArabic","NotoSansHebrew","NotoSansDevanagari","NotoSansTamil"};
    struct font_face *f=&faces[id];
    if (f->tried) return f->hb ? f : NULL;
    f->tried=1;
    char path[72]; snprintf(path,sizeof(path),"/lib/netsurf/fonts/%s.ttf",names[id]);
    int len=lx_stat(path), fd;
    if (len<=0 || len>2*1024*1024 || (fd=lx_open(path))<0) return NULL;
    char *data=malloc(len); int pos=0;
    while (data && pos<len) {
        int n=lx_read(fd,data+pos,len-pos);
        if (n<=0) break;
        pos+=n;
    }
    lx_close(fd);
    if (pos!=len || !stbtt_InitFont(&f->tt,(unsigned char *)data,0)) { free(data); return NULL; }
    hb_blob_t *blob=hb_blob_create(data,len,HB_MEMORY_MODE_READONLY,data,free);
    hb_face_t *face=hb_face_create(blob,0);
    f->units=hb_face_get_upem(face);
    f->hb=hb_font_create(face); hb_ot_font_set_funcs(f->hb);
    hb_face_destroy(face); hb_blob_destroy(blob); return f;
}
static struct {
    hb_buffer_t *buf;
    struct font_face *face;
    int px,pen;
    hb_glyph_info_t *glyphs;
    hb_glyph_position_t *positions;
} shaping;
static int rounded(int v) { return (v+32)>>6; }
/* NetSurf also measures non-NUL-terminated byte ranges. */
static unsigned run_codepoint(const char **p,const char *end) {
    unsigned lead=(unsigned char)**p;
    size_t need=lead<0xc2?1:lead<0xe0?2:lead<0xf0?3:4;
    if ((size_t)(end-*p)<need) { *p=end; return 0xfffd; }
    return utf8_next(p);
}
struct shaped_run ns_shape(const char *text,size_t len,int px) {
    struct shaped_run run={0};
    if (!len || (unsigned char)*text<0x80) return run;
    const char *p=text;
    unsigned first=run_codepoint(&p,text+len);
    int id=script_face(first);
    if (id<0 || !(shaping.face=shape_face(id))) return run;
    hb_script_t script=hb_unicode_script(hb_unicode_funcs_get_default(),first);
    while ((size_t)(p-text)<len) {
        const char *next=p; unsigned ch=run_codepoint(&next,text+len);
        bool mark=(ch>=0x300 && ch<=0x36f)||ch==0x200c||ch==0x200d;
        if (!mark && (script_face(ch)!=id || hb_unicode_script(hb_unicode_funcs_get_default(),ch)!=script)) break;
        p=next;
    }
    if (!shaping.buf) shaping.buf=hb_buffer_create();
    hb_buffer_clear_contents(shaping.buf);
    run.bytes=p-text;
    hb_buffer_add_utf8(shaping.buf,text,run.bytes,0,run.bytes);
    hb_buffer_guess_segment_properties(shaping.buf);
    hb_font_set_scale(shaping.face->hb,px*64,px*64);
    hb_shape(shaping.face->hb,shaping.buf,NULL,0);
    shaping.glyphs=hb_buffer_get_glyph_infos(shaping.buf,&run.count);
    shaping.positions=hb_buffer_get_glyph_positions(shaping.buf,NULL);
    shaping.px=px; shaping.pen=0;
    run.rtl=HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(shaping.buf));
    int advance=0;
    for (unsigned i=0;i<run.count;i++) advance+=shaping.positions[i].x_advance;
    run.width=rounded(advance); return run;
}
struct shaped_glyph ns_shaped_glyph(unsigned i) {
    static unsigned char *mask; static size_t cap;
    hb_glyph_position_t p=shaping.positions[i];
    int glyph=shaping.glyphs[i].codepoint;
    float scale=(float)shaping.px/shaping.face->units;
    int x0,y0,x1,y1;
    stbtt_GetGlyphBitmapBox(&shaping.face->tt,glyph,scale,scale,&x0,&y0,&x1,&y1);
    struct shaped_glyph g={NULL,rounded(shaping.pen+p.x_offset)+x0,y0-rounded(p.y_offset),x1-x0,y1-y0};
    shaping.pen+=p.x_advance;
    if (g.width<=0 || g.height<=0 || g.width>1024 || g.height>1024) return g;
    size_t n=g.width*g.height;
    if (n>cap) { void *tmp=realloc(mask,n); if (!tmp) return g; mask=tmp; cap=n; }
    stbtt_MakeGlyphBitmap(&shaping.face->tt,mask,g.width,g.height,g.width,scale,scale,glyph);
    g.mask=mask; return g;
}
size_t ns_shaped_caret(struct shaped_run run,int x,int *actual) {
    int pen=0;
    size_t trailing=run.rtl?run.bytes:0;
    for (unsigned i=0;i<run.count;) {
        unsigned cluster=shaping.glyphs[i].cluster, j=i;
        int start=rounded(pen);
        while (j<run.count && shaping.glyphs[j].cluster==cluster) pen+=shaping.positions[j++].x_advance;
        int end=rounded(pen);
        size_t left=run.rtl?trailing:cluster;
        trailing=run.rtl?cluster:(j<run.count?shaping.glyphs[j].cluster:run.bytes);
        if (x<=end) { bool near_left=x-start <= end-x; *actual=near_left?start:end; return near_left?left:trailing; }
        i=j;
    }
    *actual=run.width; return trailing;
}
