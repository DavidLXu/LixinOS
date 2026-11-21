// The upstream flex box tree already blockifies and normalises item boxes.
// Grid uses that tree, with shared tracks instead of per-line flex widths.
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>

#define GRID_TRACKS 128
enum grid_intrinsic { GRID_DEFINITE, GRID_MIN_CONTENT, GRID_MAX_CONTENT };
struct grid_track { double fixed, fraction; enum grid_intrinsic intrinsic; };
struct grid_axis { struct grid_track tracks[GRID_TRACKS]; int count; };

static void grid_space(const char **p) { while (isspace((unsigned char)**p)) ++*p; }
static bool grid_word(const char **p, const char *word) {
    size_t n = strlen(word);
    if (strncasecmp(*p, word, n)) return false;
    *p += n;
    return true;
}
static bool grid_track(const char **p, int available, struct grid_track *track) {
    grid_space(p);
    if (grid_word(p,"min-content")) { *track = (struct grid_track){.intrinsic=GRID_MIN_CONTENT}; return true; }
    if (grid_word(p,"max-content")) { *track = (struct grid_track){.intrinsic=GRID_MAX_CONTENT}; return true; }
    if (grid_word(p,"auto")) {
        *track = (struct grid_track){.fraction=1}; return true;
    }
    if (grid_word(p,"minmax(")) {
        struct grid_track minimum, maximum;
        /* The two arguments are breadths, never nested minmax functions. */
        grid_space(p);
        if (!strncasecmp(*p,"minmax(",7)) return false;
        if (!grid_track(p,available,&minimum)) return false;
        grid_space(p);
        if (**p != ',') return false;
        ++*p; grid_space(p);
        if (!strncasecmp(*p,"minmax(",7) || !grid_track(p,available,&maximum)) return false;
        grid_space(p);
        if (**p != ')') return false;
        ++*p; *track = maximum;
        if (minimum.fixed > track->fixed) track->fixed = minimum.fixed;
        return true;
    }
    char *end;
    double n = strtod(*p,&end);
    if (end == *p || !isfinite(n) || n < 0 || n > 1000000) return false;
    *p = end;
    if (grid_word(p,"fr")) *track = (struct grid_track){.fraction=n};
    else {
        if (grid_word(p,"%")) n *= available/100.;
        else if (!grid_word(p,"px") && n != 0) return false;
        *track = (struct grid_track){.fixed=n};
    }
    return true;
}
static bool grid_axis_parse(const char *p, int space, int gap, int items, struct grid_axis *axis) {
    while (p && *p) {
        grid_space(&p);
        if (!*p) break;
        int count = 1;
        bool repeat = grid_word(&p,"repeat("), fit = false;
        if (repeat) {
            grid_space(&p);
            if ((fit = grid_word(&p,"auto-fit")) || grid_word(&p,"auto-fill")) count = 0;
            else {
                char *end;
                long n = strtol(p,&end,10);
                if (p == end || n < 1 || n > GRID_TRACKS) return false;
                p = end; count = n;
            }
            grid_space(&p); if (*p != ',') return false; ++p;
        }
        int first = axis->count;
        do {
            if (axis->count == GRID_TRACKS ||
                !grid_track(&p,space,&axis->tracks[axis->count])) return false;
            axis->count++; grid_space(&p);
        } while (repeat && *p && *p != ')');
        if (repeat) {
            if (*p != ')') return false;
            ++p;
            int group = axis->count-first;
            if (!count) {
                double width = gap * group;
                for (int i=first;i<axis->count;i++) width += axis->tracks[i].fixed;
                double copies = width > 0 ? (space + (double)gap)/width : 1;
                count = copies >= GRID_TRACKS ? GRID_TRACKS : (int)copies;
                if (fit && count > (items+group-1)/group) count = (items+group-1)/group;
                if (count < 1) count = 1;
                if (count > (GRID_TRACKS-first)/group) count = (GRID_TRACKS-first)/group;
            }
            if (count > (GRID_TRACKS-first)/group) return false;
            for (int i=1;i<count;i++) {
                memcpy(axis->tracks+axis->count,axis->tracks+first,group*sizeof(struct grid_track));
                axis->count += group;
            }
        }
    }
    return axis->count != 0;
}

/* Resolve fr units against the remaining space. Freeze tracks whose minimum
 * already exceeds their fractional share before distributing to the others. */
static void grid_size_tracks(struct grid_axis *axis, int space, int gap) {
    double remaining = space - (double)(axis->count-1)*gap, weights = 0;
    for (int i=0;i<axis->count;i++) {
        struct grid_track *t = &axis->tracks[i];
        if (t->fraction > 0) weights += t->fraction;
        else remaining -= t->fixed;
    }
    bool changed;
    do {
        changed = false;
        double unit = remaining > 0 ? remaining / fmax(1.,weights) : 0;
        for (int i=0;i<axis->count;i++) {
            struct grid_track *t=&axis->tracks[i];
            if (t->fraction>0 && t->fixed > t->fraction*unit) {
                remaining-=t->fixed;
                weights-=t->fraction;
                t->fraction=0;
                changed=true;
            }
        }
    } while (changed);
    double unit=remaining > 0 ? remaining / fmax(1.,weights) : 0;
    for (int i=0;i<axis->count;i++) {
        struct grid_track *t=&axis->tracks[i];
        if (t->fraction>0) t->fixed=t->fraction*unit;
    }
}

static const char *grid_css(uint8_t (*get)(const css_computed_style *, lwc_string **),
                            const css_computed_style *style) {
    lwc_string *s = NULL; get(style,&s);
    return s ? lwc_string_data(s) : "";
}
static int grid_outer(struct box *b, int side) {
    return b->padding[side]+b->border[side].width+b->margin[side];
}

/* Resolve an explicit column or an auto-positioned span for this item. */
static void grid_place(const struct box *b, int count, int col, int *start, int *length) {
    int span = 1, target = col;
    const char *place = b->style ? grid_css(css_computed_grid_column,b->style) : "";
    grid_space(&place);
    if (grid_word(&place,"span")) span = atoi(place);
    else if (isdigit((unsigned char)*place)) {
        char *end; long n = strtol(place,&end,10);
        if (n >= 1 && n <= count) target = (int)n-1;
        place = end; grid_space(&place);
        if (*place == '/') { ++place; grid_space(&place);
            span = grid_word(&place,"span") ? atoi(place) : atoi(place)-target-1; }
    }
    if (span < 1) span = 1;
    if (span > count) span = count;
    *start = target;
    *length = span;
}

static bool layout_grid(struct flex_ctx *ctx, struct box *grid, int width) {
    struct grid_axis columns = {0}, rows = {0};
    css_fixed length = 0; css_unit unit = CSS_UNIT_PX;
    int colgap = 0, rowgap = 0, items = 0;
    for (struct box *b=grid->children; b; b=b->next) items++;
    if (css_computed_column_gap(grid->style,&length,&unit) == CSS_COLUMN_GAP_SET)
        colgap = unit == CSS_UNIT_PCT ? (int)(FIXTOFLT(length)*width/100) :
            FIXTOINT(css_unit_len2device_px(grid->style,ctx->unit_len_ctx,length,unit));
    const char *value = grid_css(css_computed_row_gap,grid->style);
    struct grid_track gap;
    if (grid_track(&value,width,&gap)) rowgap = gap.fixed;
    if (!grid_axis_parse(grid_css(css_computed_grid_template_columns,grid->style),
                         width,colgap,items,&columns)) {
        columns.count = 1; columns.tracks[0] = (struct grid_track){.fraction=1};
    }
    if (!grid_axis_parse(grid_css(css_computed_grid_template_rows,grid->style),
                         grid->height==AUTO ? 0 : grid->height,rowgap,items,&rows)) rows.count = 0;
    int pos[GRID_TRACKS+1], count = columns.count;
    int column = 0;
    for (struct box *b=grid->children;b;b=b->next) {
        int start, span;
        grid_place(b,count,column,&start,&span);
        if (start+span>count) start=0;
        struct grid_track *track=&columns.tracks[start];
        if (span==1 && track->intrinsic) {
            int intrinsic = track->intrinsic==GRID_MIN_CONTENT ? b->min_width : b->max_width;
            if (intrinsic>track->fixed && intrinsic!=UNKNOWN_WIDTH) track->fixed=intrinsic;
        }
        column=(start+span)%count;
    }
    grid_size_tracks(&columns,width,colgap);
    if (grid->height!=AUTO && rows.count) grid_size_tracks(&rows,grid->height,rowgap);
    double offset = grid->padding[LEFT];
    double occupied = (count-1)*colgap;
    for (int i=0;i<count;i++) occupied += columns.tracks[i].fixed;
    if (occupied < width) {
        switch (css_computed_justify_content(grid->style)) {
        case CSS_JUSTIFY_CONTENT_CENTER: offset += (width-occupied)*0.5; break;
        case CSS_JUSTIFY_CONTENT_FLEX_END: offset += width-occupied; break;
        }
    }
    for (int i=0;i<count;i++) {
        pos[i] = (int)(offset+0.5);
        offset = fmin(16000000., offset + columns.tracks[i].fixed + colgap);
    }
    pos[count] = (int)(offset+0.5);
    int y = grid->padding[TOP], col = 0, row = 0, height = 0;
    struct box *start = grid->children;
    for (struct box *b=grid->children; b; b=b->next) {
        int span, target;
        grid_place(b,count,col,&target,&span);
        if (target < col || target+span > count) {
            y += height+rowgap; height = 0; row++;
            start=b; target = target<col ? target : 0;
        }
        col=target;
        int cell = pos[col+span]-pos[col]-colgap, maxw,minw,maxh,minh;
        struct flex_item_data item = {.box=b};
        b->float_container=grid;
        layout_find_dimensions(ctx->unit_len_ctx,cell,-1,b,b->style,&b->width,&b->height,
            &maxw,&minw,&maxh,&minh,b->margin,b->padding,b->border);
        b->float_container=NULL;
        for (int s=0;s<4;s++) if (b->margin[s]==AUTO) b->margin[s]=0;
        if (b->width==AUTO) b->width=cell-grid_outer(b,LEFT)-grid_outer(b,RIGHT);
        if (maxw>=0 && b->width>maxw) b->width=maxw;
        if (b->width<minw) b->width=minw;
        if (b->width<0) b->width=0;
        if (!layout_flex_item(ctx,&item,b->width)) return false;
        b->x=pos[col]+b->margin[LEFT]+b->border[LEFT].width;
        b->y=y+b->margin[TOP]+b->border[TOP].width;
        int outer=b->height+grid_outer(b,TOP)+grid_outer(b,BOTTOM);
        if (outer>height) height=outer;
        if (row<rows.count && rows.tracks[row].fixed>height) height=rows.tracks[row].fixed;
        col += span;
        if (col==count || !b->next) {
            for (struct box *c=start;c;c=c->next) {
                if (c->style && css_computed_height(c->style,&length,&unit)==CSS_HEIGHT_AUTO)
                    c->height=height-grid_outer(c,TOP)-grid_outer(c,BOTTOM);
                if (c==b) break;
            }
            y+=height+(b->next?rowgap:0); col=0; height=0; row++; start=b->next;
        }
    }
    if (grid->height==AUTO) grid->height=y-grid->padding[TOP];
    return true;
}
