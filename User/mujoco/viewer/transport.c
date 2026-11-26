#include "window-app.h"

/* Keep the binary Win3 channel independent of upstream stdio diagnostics. */
int win3_receive(void *message, unsigned size) {
    char *p = message;
    while (size) {
        int got = lx_read(0, p, size);
        if (got <= 0) return 0;
        size -= got;
        p += got;
    }
    return 1;
}
int win3_send(const void *data, unsigned size) { return app_send(data, size); }
