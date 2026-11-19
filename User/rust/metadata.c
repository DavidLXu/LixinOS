#include "../console.h"
#include "../tcc/lixin.h"
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#define PAGE 4096UL
#define BASE 0xc0000000UL
static unsigned char used[(128UL << 20) / PAGE];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *lx_rmeta_alloc(size_t len) {
    if (!len || len > sizeof(used) * PAGE) return NULL;
    size_t pages = (len + PAGE - 1) / PAGE;
    void *ptr = NULL;
    pthread_mutex_lock(&lock);
    for (size_t i = 0, run = 0; i < sizeof(used); ++i) {
        run = used[i] ? 0 : run + 1;
        if (run != pages) continue;
        if ((uintptr_t)console_call(52, (i+1)*PAGE) == BASE) {
            size_t start = i+1-pages;
            memset(used+start, 1, pages);
            ptr = (void *)(BASE + start*PAGE);
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return ptr;
}

void lx_rmeta_free(void *ptr, size_t len) {
    pthread_mutex_lock(&lock);
    memset(used + ((uintptr_t)ptr - BASE)/PAGE, 0, (len + PAGE - 1)/PAGE);
    pthread_mutex_unlock(&lock);
}

extern int lx_child_descriptor(int fd, int standard);
int lx_macro_pipe(int fd, void *data, size_t len, int writing) {
    int raw = lx_child_descriptor(fd, -1);
    if (raw < 3) return -1;
    int count = len > 4096 ? 4096 : (int)len;
    return writing ? lx_write(raw, data, count) : lx_read(raw, data, count);
}
