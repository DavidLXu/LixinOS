#pragma once
#include "load.h"
#include "window.h"

enum { PICTURE_COUNT=32, PICTURE_MARK=0xf0000, PICTURE_ROWS=128 };
struct picture {
    unsigned char *pixels;
    char *source;
    int requested_width;
    int width, height, shown_width, shown_height;
};
struct page_media {
    struct picture images[PICTURE_COUNT];
    int count;
    size_t pixels;
    char *html, *title, *headers;
    int script_errors;
    char first_error[128];
    char navigate[ADDRESS_LIMIT];
    int replace;
    struct document *runtime;
};
const char *prepare_page(struct page_source *, const char *url, int graphics_width, struct page_media *);
void release_page(struct page_media *);
int draw_picture(struct browser_window *, struct page_media *, unsigned ch, int col, int row);

int page_tick(struct page_media *);
int page_click(struct page_media *,int attribute_offset);
int page_snapshot(struct page_media *);
