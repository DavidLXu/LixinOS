/* The Links desktop already links this decoder through links/image.c.
 * NetSurf and the in-guest single-file build only need the PNG/JPEG code. */
#ifdef LX_NETSURF
unsigned char *desktop_image_rgba(unsigned char *,int,int,int,int *,int *);
#else
#define STBI_NO_STDIO
#ifndef LX_DESKTOP_BROWSER
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 16384
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#endif
#include "../gui/stb_image.h"

#endif

static uint32_t *load_scaled_image(const char *path,int width,int height,int fit)
{
    const int64_t max_pixels = 16 * 1024 * 1024;
    const long max_compressed = 32L * 1024 * 1024;
    const uint32_t background = fit ? PAPER : 0x26494cU;
    static const unsigned char png_signature[8] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };

    if (path == NULL || width <= 0 || height <= 0)
        return NULL;
    if ((int64_t)width * height > max_pixels)
        return NULL;

    long file_size = lx_stat(path);
    if (file_size < 4 || file_size > max_compressed)
        return NULL;

    int data_size = (int)file_size;
    unsigned char *data = (unsigned char *)malloc((size_t)data_size);
    if (data == NULL)
        return NULL;

    int fd = lx_open(path);
    if (fd < 0) {
        free(data);
        return NULL;
    }

    int total = 0;
    while (total < data_size) {
        int n = lx_read(fd, data + total, data_size - total);
        if (n <= 0)
            break;
        if (n > data_size - total) {
            total = -1;
            break;
        }
        total += n;
    }
    lx_close(fd);

    if (total != data_size) {
        free(data);
        return NULL;
    }

    int is_png = data_size >= 8 &&
                 memcmp(data, png_signature, sizeof(png_signature)) == 0;
    int is_jpeg = data_size >= 3 &&
                  data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
    if (!is_png && !is_jpeg) {
        free(data);
        return NULL;
    }

    int src_w=0,src_h=0;
#ifdef LX_NETSURF
    unsigned char *rgba=desktop_image_rgba(data,data_size,is_png,max_pixels,&src_w,&src_h);
#else
    int components=0;
    unsigned char *rgba=NULL;
    if (stbi_info_from_memory(data,data_size,&src_w,&src_h,&components) &&
        src_w>0 && src_h>0 && (int64_t)src_w*src_h<=max_pixels)
        rgba=stbi_load_from_memory(data,data_size,&src_w,&src_h,&components,4);
#endif
    free(data);
    if (rgba == NULL)
        return NULL;
    if (src_w <= 0 || src_h <= 0 ||
        (int64_t)src_w * src_h > max_pixels) {
        free(rgba);
        return NULL;
    }

    uint32_t *out = (uint32_t *)malloc((size_t)width * (size_t)height *
                                       sizeof(uint32_t));
    if (out == NULL) {
        free(rgba);
        return NULL;
    }

    int64_t ow = width;
    int64_t oh = height;
    int64_t sw = src_w;
    int64_t sh = src_h;
    int pad_x=0,pad_y=0;
    if (fit) {
        if (ow*sh>oh*sw) ow=oh*sw/sh;
        else oh=ow*sh/sw;
        if (ow<1) ow=1;
        if (oh<1) oh=1;
        pad_x=(width-ow)/2;
        pad_y=(height-oh)/2;
        for (int i=0;i<width*height;i++) out[i]=background;
    }
    int width_dominant = (ow * sh >= oh * sw);
    int64_t x_base = 0;
    int64_t y_base = 0;
    if (width_dominant)
        y_base = (sh * ow - oh * sw) / 2;
    else
        x_base = (sw * oh - ow * sh) / 2;

    unsigned int bg_r = (background >> 16) & 0xffU;
    unsigned int bg_g = (background >> 8) & 0xffU;
    unsigned int bg_b = background & 0xffU;

    for (int y = 0; y < oh; y++) {
        int64_t sy;
        if (width_dominant)
            sy = ((int64_t)y * sw + y_base) / ow;
        else
            sy = (int64_t)y * sh / oh;

        const unsigned char *src_row = rgba + (size_t)sy * src_w * 4;
        uint32_t *dst_row = out + (size_t)(y+pad_y) * width + pad_x;

        for (int x = 0; x < ow; x++) {
            int64_t sx;
            if (width_dominant)
                sx = (int64_t)x * sw / ow;
            else
                sx = ((int64_t)x * sh + x_base) / oh;

            const unsigned char *p = src_row + (size_t)sx * 4;
            unsigned int a = p[3];
            unsigned int inv = 255U - a;
            unsigned int r = (p[0] * a + bg_r * inv + 127U) / 255U;
            unsigned int g = (p[1] * a + bg_g * inv + 127U) / 255U;
            unsigned int b = (p[2] * a + bg_b * inv + 127U) / 255U;

            dst_row[x] = (r << 16) | (g << 8) | b;
        }
    }

    free(rgba);
    return out;
}

static uint32_t *load_wall_image(const char *path,int width,int height) {
    return load_scaled_image(path,width,height,0);
}
