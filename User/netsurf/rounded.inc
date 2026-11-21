/* Rasterize uniform solid borders and fills with one circular or elliptical
 * corner radius. All coordinates are device pixels, with exclusive ends. */
static int corner_inset(int row, int height, double rx, double ry)
{
    double edge = (row < height / 2 ? row : height - row - 1) + 0.5;
    if (edge >= ry || rx <= 0) return 0;
    double t = 1 - edge / ry;
    return (int)ceil(rx * (1 - sqrt(1 - t*t)) - 0.5);
}

static bool rounded_span(const struct redraw_context *ctx, int left, int right,
        int y, const struct rect *clip, css_color color)
{
    struct rect line = { fmax(left,clip->x0), y, fmin(right,clip->x1), y+1 };
    if (line.x0 >= line.x1 || nscss_color_is_transparent(color)) return true;
    plot_style_t style = { .fill_type=PLOT_OP_TYPE_SOLID, .fill_colour=nscss_color_to_ns(color) };
    return ctx->plot->rectangle(ctx, &style, &line) == NSERROR_OK;
}

static bool rounded_box(const struct box *box, struct rect bounds, const struct rect *clip,
        float scale, const css_unit_ctx *units, colour *background,
        const struct redraw_context *ctx, bool *painted)
{
    css_fixed radius;
    css_unit unit;
    if (!box->style || !box->parent || !box->parent->parent || box->background ||
        (box->type != BOX_BLOCK && box->type != BOX_INLINE_BLOCK &&
         box->type != BOX_FLEX && box->type != BOX_INLINE_FLEX) ||
        (html_redraw_printing && nsoption_bool(remove_backgrounds))) return true;
    css_computed_border_radius(box->style,&radius,&unit);
    if (radius <= 0) return true;
    const struct box_border *border=&box->border[TOP];
    if (border->width && border->style != CSS_BORDER_STYLE_SOLID) return true;
    for (int i=1;i<4;i++) {
        if (box->border[i].width != border->width || (border->width &&
            (box->border[i].style != border->style || box->border[i].c != border->c))) return true;
    }
    int w=bounds.x1-bounds.x0, h=bounds.y1-bounds.y0, bw=border->width*scale;
    if (w<=0 || h<=0) return true;
    double rx, ry;
    if (unit == CSS_UNIT_PCT) {
        double fraction=fmin(FIXTOFLT(radius)/100.,0.5);
        rx=w*fraction; ry=h*fraction;
    } else {
        rx=ry=fmin(FIXTOFLT(css_unit_len2device_px(box->style,units,radius,unit))*scale,
                  fmin(w,h)*0.5);
    }
    if (rx<=0 || ry<=0) return true;
    css_color fill;
    css_computed_background_color(box->style,&fill);
    if (!nscss_color_is_transparent(fill)) *background=nscss_color_to_ns(fill);
    *painted=true;
    for (int y=fmax(bounds.y0,clip->y0);y<fmin(bounds.y1,clip->y1);y++) {
        int row=y-bounds.y0, inset=corner_inset(row,h,rx,ry);
        int left=bounds.x0+inset, right=bounds.x1-inset;
        if (!rounded_span(ctx,left,right,y,clip,fill)) return false;
        if (bw==0 || nscss_color_is_transparent(border->c)) continue;
        if (row<bw || row>=h-bw || w<=2*bw) {
            if (!rounded_span(ctx,left,right,y,clip,border->c)) return false;
        } else {
            int inner=bw+corner_inset(row-bw,h-2*bw,fmax(0,rx-bw),fmax(0,ry-bw));
            if (!rounded_span(ctx,left,bounds.x0+inner,y,clip,border->c) ||
                !rounded_span(ctx,bounds.x1-inner,right,y,clip,border->c)) return false;
        }
    }
    return true;
}
