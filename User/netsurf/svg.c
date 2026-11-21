// NetSurf image handler shared by remote, file, data and inline SVG resources.
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <plutosvg.h>
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "content/llcache.h"
#include "content/content.h"
#include "utils/utils.h"
#include "netsurf/bitmap.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

struct svg_image {
    struct content base;
    plutosvg_document_t *doc;
    struct bitmap *bitmap;
    int width, height;
};

static bool decode(struct content *c) {
    struct svg_image *svg=(struct svg_image *)c;
    size_t len;
    const uint8_t *src=content__get_source_data(c,&len);
    if (!src || !len || len>4*1024*1024) goto invalid;
    svg->doc=plutosvg_document_load_from_data((const char *)src,(int)len,-1,-1,NULL,NULL);
    if (!svg->doc) goto invalid;
    float w=plutosvg_document_get_width(svg->doc), h=plutosvg_document_get_height(svg->doc);
    if (!(w>0 && w<=65535 && h>0 && h<=65535)) goto invalid;
    c->width=(int)ceilf(w); c->height=(int)ceilf(h);
    content_set_ready(c); content_set_done(c); content_set_status(c,"");
    return true;
invalid:
    content_broadcast_error(c,NSERROR_SVG_ERROR,NULL);
    return false;
}
static nserror create(const content_handler *handler,lwc_string *mime,const struct http_parameter *params,
                      struct llcache_handle *llcache,const char *charset,bool quirks,struct content **out) {
    struct svg_image *svg=calloc(1,sizeof(*svg));
    if (!svg) return NSERROR_NOMEM;
    nserror err=content__init(&svg->base,handler,mime,params,llcache,charset,quirks);
    if (err!=NSERROR_OK) { free(svg); return err; }
    *out=&svg->base; return NSERROR_OK;
}
static void destroy(struct content *c) {
    struct svg_image *svg=(struct svg_image *)c;
    if (svg->bitmap) guit->bitmap->destroy(svg->bitmap);
    if (svg->doc) plutosvg_document_destroy(svg->doc);
}
static nserror clone_image(const struct content *old,struct content **out) {
    struct svg_image *svg=calloc(1,sizeof(*svg));
    if (!svg) return NSERROR_NOMEM;
    nserror err=content__clone(old,&svg->base);
    if (err==NSERROR_OK && (old->status==CONTENT_STATUS_READY || old->status==CONTENT_STATUS_DONE) &&
        !decode(&svg->base)) err=NSERROR_CLONE_FAILED;
    if (err!=NSERROR_OK) { content_destroy(&svg->base); return err; }
    *out=&svg->base; return NSERROR_OK;
}
static struct bitmap *raster(struct svg_image *svg,int w,int h) {
    if (!svg->doc || w<=0 || h<=0) return NULL;
    // Cap the cached raster independently of the CSS display dimensions.
    double ratio=fmin(1.,fmin(4096./fmax(w,h),sqrt(4194304./((double)w*h))));
    w=fmax(1,floor(w*ratio)); h=fmax(1,floor(h*ratio));
    if (svg->bitmap && svg->width==w && svg->height==h) return svg->bitmap;
    struct bitmap *bm=guit->bitmap->create(w,h,BITMAP_CLEAR);
    if (!bm) return NULL;
    plutovg_surface_t *surface=plutovg_surface_create_for_data(guit->bitmap->get_buffer(bm),
            w,h,guit->bitmap->get_rowstride(bm));
    plutovg_canvas_t *canvas=surface ? plutovg_canvas_create(surface) : NULL;
    bool ok=false;
    if (canvas) {
        plutovg_canvas_scale(canvas,w/plutosvg_document_get_width(svg->doc),h/plutosvg_document_get_height(svg->doc));
        ok=plutosvg_document_render(svg->doc,NULL,canvas,NULL,NULL,NULL);
        plutovg_canvas_destroy(canvas);
    }
    if (surface) plutovg_surface_destroy(surface);
    if (!ok) { guit->bitmap->destroy(bm); return NULL; }
    bitmap_format_to_client(bm,&(bitmap_fmt_t){.layout=BITMAP_LAYOUT_ARGB8888,.pma=true});
    guit->bitmap->modified(bm);
    if (svg->bitmap) guit->bitmap->destroy(svg->bitmap);
    svg->bitmap=bm; svg->width=w; svg->height=h;
    return bm;
}
static bool redraw(struct content *c,struct content_redraw_data *d,const struct rect *clip,
                   const struct redraw_context *ctx) {
    if (d->width<=0 || d->height<=0) return true;
    struct bitmap *bm=raster((struct svg_image *)c,d->width,d->height);
    bitmap_flags_t flags=(d->repeat_x?BITMAPF_REPEAT_X:0)|(d->repeat_y?BITMAPF_REPEAT_Y:0);
    return bm && ctx->plot->bitmap(ctx,bm,d->x,d->y,d->width,d->height,d->background_colour,flags)==NSERROR_OK;
}
static void *internal(const struct content *c,void *ctx) {
    return raster((struct svg_image *)c,c->width,c->height);
}
static content_type image_type(void) { return CONTENT_IMAGE; }
static const content_handler image_handler={.create=create,.data_complete=decode,.destroy=destroy,
    .redraw=redraw,.clone=clone_image,.get_internal=internal,.type=image_type,.no_share=false};
static const char *types[]={"image/svg+xml"};
CONTENT_FACTORY_REGISTER_TYPES(lx_svg,types,image_handler);
