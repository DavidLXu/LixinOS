// Keep DOM/layout order intact. The renderer and pointer walk share a stable
// sibling paint list built after reflow, with raised descendants of auto layers.
static struct box *merge_paint(struct box *a, struct box *b) {
    struct box *head=NULL, **tail=&head;
    while (a && b) {
        struct box **next = a->paint_z <= b->paint_z ? &a : &b;
        *tail=*next; *next=(*next)->paint_next; tail=&(*tail)->paint_next;
    }
    *tail=a?a:b;
    return head;
}
static struct box *sort_paint(struct box *first) {
    if (!first || !first->paint_next) return first;
    struct box *mid=first,*fast=first->paint_next;
    while (fast && fast->paint_next) { mid=mid->paint_next; fast=fast->paint_next->paint_next; }
    struct box *second=mid->paint_next; mid->paint_next=NULL;
    return merge_paint(sort_paint(first),sort_paint(second));
}
static void prepare_paint(struct box *box) {
    int z=0, previous=INT_MIN;
    bool sorted=true;
    box->paint_first=box->children;
    for (struct box *c=box->children;c;c=c->next) {
        translate_box(c);
        prepare_paint(c);
        c->paint_next=c->next;
        if (c->paint_z<previous) sorted=false;
        previous=c->paint_z;
        if (previous>z) z=previous;
    }
    if (!sorted) box->paint_first=sort_paint(box->paint_first);
    int32_t own;
    bool positioned=box->style && (css_computed_position(box->style)!=CSS_POSITION_STATIC ||
            (box->parent && (box->parent->type==BOX_FLEX || box->parent->type==BOX_INLINE_FLEX)));
    box->paint_z=positioned && css_computed_z_index(box->style,&own)==CSS_Z_INDEX_SET ? own : z;
}
