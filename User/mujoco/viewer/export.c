#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int lx_save_buffer(const char *, const void *, unsigned);

/* Each regular-file write commits a guest filesystem snapshot. Collect one
 * export in memory so fclose publishes it in a single atomic transaction. */
struct export_file {
    char *path;
    unsigned char *bytes;
    size_t size, capacity;
    int failed;
};

static ssize_t append(void *cookie, const char *bytes, size_t count) {
    struct export_file *file=cookie;
    if (count > UINT_MAX-file->size) {
        errno=EFBIG;
        file->failed=1;
        return -1;
    }
    size_t required=file->size+count;
    if (required>file->capacity) {
        size_t capacity=file->capacity ? file->capacity*2 : 4096;
        if (capacity<required) capacity=required;
        unsigned char *next=realloc(file->bytes,capacity);
        if (!next) { file->failed=1; return -1; }
        file->bytes=next;
        file->capacity=capacity;
    }
    memcpy(file->bytes+file->size,bytes,count);
    file->size=required;
    return count;
}

static int publish(void *cookie) {
    struct export_file *file=cookie;
    int ok=!file->failed && lx_save_buffer(file->path,file->bytes,file->size)==0;
    if (!ok) fprintf(stderr,"MuJoCo could not save %s\n",file->path);
    free(file->bytes);
    free(file->path);
    free(file);
    if (!ok) errno=EIO;
    return ok ? 0 : -1;
}

FILE *win3_export_open(const char *path, const char *mode) {
    if (strcmp(mode,"w") && strcmp(mode,"wb") && strcmp(mode,"wt")) return fopen(path,mode);
    struct export_file *file=calloc(1,sizeof *file);
    if (!file) return NULL;
    file->path=strdup(path);
    cookie_io_functions_t io={.write=append,.close=publish};
    FILE *stream=file->path ? fopencookie(file,"w",io) : NULL;
    if (!stream) { free(file->path); free(file); }
    return stream;
}
