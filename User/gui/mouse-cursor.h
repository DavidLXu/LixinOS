#ifndef LIXIN_MOUSE_CURSOR_H
#define LIXIN_MOUSE_CURSOR_H

enum { CUR_W = 16, CUR_H = 21 };

/* The top-left pixel is the hotspot. Rust reads these same quoted rows. */
static const char lixin_cursor[CUR_H][CUR_W + 1] = {
    "O...............",
    "OO..............",
    "OWO~............",
    "OWWO~...........",
    "OWWWG+..........",
    "OWWWWG+.........",
    "OWWWWWgO........",
    "OWWWWWWWO~......",
    "+WWWWWWWWO~.....",
    "+WWWWWWWWWG+....",
    "+WWWWWWWWWWG+...",
    "+WWWWWWGOOOO++..",
    "+WWWWWWG~::::::.",
    "+WWgOGWWO.......",
    "+ggO:+WWg+......",
    "+GO::~OWWO:.....",
    "~O::..+gWW+.....",
    "~::....OWWO~....",
    ".......+gGO~....",
    "........O~::....",
    ".........:......",
};

/* Text hotspot is (4, 7). */
static const char lixin_text_cursor[CUR_H][CUR_W + 1] = {
    ".OOOOOOO........",
    ".OWWWWWO........",
    ".OOOWOOO........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    "...OWO..........",
    ".OOOWOOO........",
    ".OWWWWWO........",
    ".OOOOOOO........",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
};

static inline unsigned lixin_cursor_pixel(char ink, unsigned background) {
    const unsigned outline = 0x1b2532, shadow = 0x0c1219;
    switch (ink) {
    case 'O': return outline;
    case 'G': return 0x627180;
    case 'g': return 0xaebdca;
    case 'W': return 0xf7fbff;
    case '+': return ((background & 0xfefefe) >> 1) + ((outline & 0xfefefe) >> 1);
    case '~': return background - ((background & 0xfcfcfc) >> 2) + ((outline & 0xfcfcfc) >> 2);
    case ':': return background - ((background & 0xfcfcfc) >> 2) + ((shadow & 0xfcfcfc) >> 2);
    default: return background;
    }
}

#endif
