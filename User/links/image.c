#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "../gui/stb_image.h"
#include "page.h"

int decode_picture(struct page_media *media,const struct page_source *src,int available,int wanted) {
    struct picture *im=&media->images[media->count];
    int w,h,n;
    if (!stbi_info_from_memory((unsigned char *)src->body,src->length,&w,&h,&n) ||
        w<=0 || h<=0 || (size_t)w*h > 4194304-media->pixels) return 0;
    im->pixels=stbi_load_from_memory((unsigned char *)src->body,src->length,&w,&h,&n,4);
    if (!im->pixels) return 0;
    im->width=w; im->height=h;
    im->shown_width=wanted>0?wanted:w;
    if (im->shown_width>available) im->shown_width=available;
    im->shown_height=(long)h*im->shown_width/w;
    if (im->shown_height>WEB_LINE*PICTURE_ROWS) {
        im->shown_height=WEB_LINE*PICTURE_ROWS;
        im->shown_width=(long)w*im->shown_height/h;
    }
    if (!im->shown_width) im->shown_width=1;
    if (!im->shown_height) im->shown_height=1;
    media->pixels+=(size_t)w*h;
    media->count++;
    return 1;
}
int draw_picture(struct browser_window *win,struct page_media *media,unsigned ch,int col,int row) {
    unsigned marker=ch-PICTURE_MARK;
    if (marker >= (unsigned)media->count*PICTURE_ROWS) return 0;
    struct picture *im=&media->images[marker/PICTURE_ROWS];
    int offset=(marker%PICTURE_ROWS)*WEB_LINE;
    int left=WEB_MARGIN+col*WEB_CHAR, top=WEB_TOP+row*WEB_LINE;
    for (int dy=0;dy<WEB_LINE && offset+dy<im->shown_height;dy++) {
        unsigned sy=(offset+dy)*im->height/im->shown_height;
        unsigned *dest=win->canvas.pixels+(win->y+top+dy)*win->canvas.width+win->x+left;
        for (int x=0;x<im->shown_width && left+x<win->w-WEB_MARGIN;x++) {
            unsigned sx=x*im->width/im->shown_width;
            unsigned char *p=im->pixels+4*(sy*im->width+sx);
            unsigned a=p[3], white=255*(255-a);
            dest[x]=((p[0]*a+white)/255<<16)|((p[1]*a+white)/255<<8)|((p[2]*a+white)/255);
        }
    }
    return 1;
}
