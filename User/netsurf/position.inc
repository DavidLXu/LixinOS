// Percentage insets use the containing box. Percentage translations use
// the element's own border box, which can be much wider than the viewport.
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <math.h>

static const char *position_value(const css_computed_style *s,
        uint8_t (*get)(const css_computed_style *, lwc_string **)) {
    lwc_string *v=NULL; get(s,&v); return v ? lwc_string_data(v) : "";
}
static bool position_length(const char **s,int basis,int *value) {
    char *end;
    double n=strtod(*s,&end);
    if (end==*s || !isfinite(n)) return false;
    if (*end=='%') { n=n*basis/100.; end++; }
    else if (!strncasecmp(end,"px",2)) end+=2;
    else if (n!=0) return false;
    if (fabs(n)>16000000) return false;
    *value=(int)round(n);
    while (isspace((unsigned char)*end)) end++;
    *s=end; return true;
}
static void logical_insets(const struct box *b,int width,int *left,int *right) {
    bool rtl=css_computed_direction(b->style)==CSS_DIRECTION_RTL;
    const char *start=position_value(b->style,css_computed_inset_inline_start);
    const char *end=position_value(b->style,css_computed_inset_inline_end);
    int v;
    if (position_length(&start,width,&v) && !*start) *(rtl?right:left)=v;
    if (position_length(&end,width,&v) && !*end) *(rtl?left:right)=v;
}
static void translate_box(struct box *b) {
    if (!b->style) return;
    const char *p=position_value(b->style,css_computed_transform);
    int w=b->width+b->padding[LEFT]+b->padding[RIGHT]+b->border[LEFT].width+b->border[RIGHT].width;
    int h=b->height+b->padding[TOP]+b->padding[BOTTOM]+b->border[TOP].width+b->border[BOTTOM].width;
    int dx=0,dy=0;
    while (*p) {
        int x=0,y=0;
        if (!strncasecmp(p,"translate(",10)) {
            p+=10; if (!position_length(&p,w,&x)) return;
            if (*p==',') { ++p; if (!position_length(&p,h,&y)) return; }
        } else if (!strncasecmp(p,"translateX(",11)) {
            p+=11; if (!position_length(&p,w,&x)) return;
        } else if (!strncasecmp(p,"translateY(",11)) {
            p+=11; if (!position_length(&p,h,&y)) return;
        } else return;
        if (*p!=')') return;
        ++p; while (isspace((unsigned char)*p)) ++p;
        dx+=x;dy+=y;
    }
    b->x+=dx; b->y+=dy;
}
