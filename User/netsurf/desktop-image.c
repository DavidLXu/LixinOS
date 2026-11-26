/* Share NetSurf's existing codecs with the file manager and wallpaper loader. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>

struct jpeg_state {
    struct jpeg_decompress_struct info;
    struct jpeg_error_mgr error;
    jmp_buf jump;
    unsigned char *pixels;
};

static void jpeg_failed(j_common_ptr info) {
    struct jpeg_state *state=info->client_data;
    longjmp(state->jump,1);
}

static unsigned char *read_jpeg(unsigned char *data,int size,int limit,int *w,int *h) {
    struct jpeg_state *state=calloc(1,sizeof *state);
    if (!state) return NULL;
    struct jpeg_decompress_struct *info=&state->info;
    info->err=jpeg_std_error(&state->error);
    state->error.error_exit=jpeg_failed;
    info->client_data=state;
    if (setjmp(state->jump)) {
        jpeg_destroy_decompress(info);
        free(state->pixels);
        free(state);
        return NULL;
    }
    jpeg_create_decompress(info);
    jpeg_mem_src(info,data,size);
    jpeg_read_header(info,TRUE);
    if (!info->image_width || !info->image_height ||
        (uint64_t)info->image_width*info->image_height>(unsigned)limit) goto done;
    int cmyk=info->jpeg_color_space==JCS_CMYK || info->jpeg_color_space==JCS_YCCK;
    info->out_color_space=cmyk?JCS_CMYK:JCS_RGB;
    jpeg_start_decompress(info);
    *w=info->output_width;
    *h=info->output_height;
    state->pixels=malloc((size_t)*w * *h * 4);
    if (!state->pixels) goto done;
    JSAMPARRAY row=(*info->mem->alloc_sarray)((j_common_ptr)info,JPOOL_IMAGE,
                                                *w*info->output_components,1);
    while (info->output_scanline<info->output_height) {
        unsigned char *dst=state->pixels+(size_t)info->output_scanline * *w * 4;
        if (jpeg_read_scanlines(info,row,1)!=1) jpeg_failed((j_common_ptr)info);
        const unsigned char *src=row[0];
        for (int x=0;x<*w;x++,src+=info->output_components,dst+=4) {
            for (int c=0;c<3;c++) {
                if (!cmyk) dst[c]=src[c];
                else if (info->saw_Adobe_marker) dst[c]=src[c]*src[3]/255;
                else dst[c]=(255-src[c])*(255-src[3])/255;
            }
            dst[3]=255;
        }
    }
    jpeg_finish_decompress(info);
done:
    jpeg_destroy_decompress(info);
    unsigned char *pixels=state->pixels;
    free(state);
    return pixels;
}

unsigned char *desktop_image_rgba(unsigned char *data,int size,int png,int limit,int *w,int *h) {
    if (!png) return read_jpeg(data,size,limit,w,h);
    png_image img={0};
    img.version=PNG_IMAGE_VERSION;
    unsigned char *rgba=NULL;
    if (png_image_begin_read_from_memory(&img,data,size) && img.width && img.height &&
        (uint64_t)img.width*img.height<=(unsigned)limit) {
        *w=img.width; *h=img.height;
        img.format=PNG_FORMAT_RGBA;
        rgba=malloc(PNG_IMAGE_SIZE(img));
        if (rgba && !png_image_finish_read(&img,NULL,rgba,0,NULL)) {
            free(rgba); rgba=NULL;
        }
    }
    png_image_free(&img);
    return rgba;
}
