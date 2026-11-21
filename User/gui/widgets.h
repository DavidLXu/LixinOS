#ifndef LX_WIDGETS_H
#define LX_WIDGETS_H
#include <string.h>

/* All positions in a text field are UTF-8 byte boundaries. Feed keyboard and
 * paste bytes through the same function; incomplete glyphs stay off screen. */
typedef struct { int caret, anchor, pending, needed; unsigned char bytes[4]; } UiText;
enum { UI_LEFT = 300, UI_RIGHT, UI_HOME = 304, UI_END, UI_DELETE };
static inline void ui_text_begin(UiText *t, const char *s, int select_all) {
    t->caret = strlen(s);
    t->anchor = select_all ? 0 : t->caret;
    t->pending = 0;
}
static inline int ui_prev(const char *s, int i) {
    if (i) while (--i && ((unsigned char)s[i] & 192) == 128) {}
    return i;
}
static inline int ui_next(const char *s, int i) {
    if (s[i]) while (((unsigned char)s[++i] & 192) == 128) {}
    return i;
}
static inline int ui_text_key(UiText *t, char *s, int capacity, int key) {
    int lo = t->caret < t->anchor ? t->caret : t->anchor;
    int hi = t->caret + t->anchor - lo, len = strlen(s), n = 0;
    if (key >= 128 && key <= 255) {
        if (key >= 192) {
            t->pending = 0;
            if (key < 194 || key > 244) return 0;
            t->needed = key < 224 ? 2 : key < 240 ? 3 : 4;
        } else if (!t->pending) return 0;
        else if (t->pending == 1 && ((t->bytes[0]==224 && key<160) ||
                 (t->bytes[0]==237 && key>=160) || (t->bytes[0]==240 && key<144) ||
                 (t->bytes[0]==244 && key>=144))) { t->pending=0; return 0; }
        t->bytes[t->pending++] = key;
        if (t->pending != t->needed) return 0;
        n = t->pending;
    } else if (key >= 32 && key < 127) { t->bytes[0] = key; n = 1; }
    t->pending = 0;
    if (key == 1) { t->caret=len; t->anchor=0; return 1; }
    if (key == UI_LEFT || key == UI_RIGHT || key == UI_HOME || key == UI_END) {
        t->caret = key == UI_HOME ? 0 : key == UI_END ? len : key == UI_LEFT ?
            (lo != hi ? lo : ui_prev(s,lo)) : (lo != hi ? hi : ui_next(s,hi));
        t->anchor=t->caret; return 1;
    }
    if (key == 21) { lo=0; hi=len; }
    else if (key == 8 || key == 127) { if (lo==hi) lo=ui_prev(s,lo); }
    else if (key == UI_DELETE) { if (lo==hi) hi=ui_next(s,hi); }
    else if (!n) return 0;
    if (len-hi+lo+n >= capacity) return 0;
    memmove(s+lo+n,s+hi,len-hi+1);
    if (n) memcpy(s+lo,t->bytes,n);
    t->anchor=t->caret=lo+n;
    return 1;
}

static inline int ui_text_paste(UiText *t,char *s,int capacity,const unsigned char *bytes,int length) {
    int changed=0;
    t->pending=0;
    for (int i=0;i<length;++i) {
        unsigned char b=bytes[i];
        if (b=='\r' || b=='\n') break;
        if (b>=32 && b!=127) changed |= ui_text_key(t,s,capacity,b);
    }
    t->pending=0;
    return changed;
}

/* IDs start at one. Hit-test against the same bounds used when painting.
 * Releasing outside the originally pressed control cancels activation. */
typedef struct { int focus, pressed, down; } UiControls;
static inline int ui_pointer(UiControls *u, int hit, int down) {
    int action=0;
    if (down && !u->down) { u->pressed=hit; if (hit) u->focus=hit; }
    if (!down && u->down) { if (hit==u->pressed) action=hit; u->pressed=0; }
    u->down=down;
    return action;
}
static inline int ui_focus_next(int focus, const int *ids, int n) {
    for (int i=0;i<n;++i) if (ids[i]==focus) return ids[(i+1)%n];
    return n ? ids[0] : 0;
}
static inline int ui_limit(int v,int max) { return v<0 || max<0 ? 0 : v>max ? max : v; }
static inline int ui_list_top(int first,int count,int visible) {
    return ui_limit(first,count-visible);
}
static inline int ui_list_reveal(int first,int selected,int count,int visible) {
    if (selected>=0 && selected<first) first=selected;
    if (selected>=first+visible) first=selected-visible+1;
    return ui_list_top(first,count,visible);
}
typedef struct { int start,length,travel; } UiThumb;
static inline UiThumb ui_list_thumb(int top,int count,int visible,int track) {
    int length=count>visible ? (long)track*visible/count : track;
    if (length<15) length=15;
    length=ui_limit(length,track);
    int travel=track-length;
    return (UiThumb){count>visible ? (int)((long)travel*ui_list_top(top,count,visible)/(count-visible)) : 0,length,travel};
}
static inline int ui_list_drag(int offset,int travel,int count,int visible) {
    return travel>0 && count>visible ? (long)ui_limit(offset,travel)*(count-visible)/travel : 0;
}
#endif
