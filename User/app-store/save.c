/* Save one checked workbench payload in a single LixinOS filesystem commit. */
#include <lixin.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    static char data[32768];
    if (argc != 3 || (strcmp(argv[1], "app.c") && strcmp(argv[1], "tests.json") &&
                      strcmp(argv[1], ".desktop/candidate"))) return 2;
    char *end;
    long len = strtol(argv[2], &end, 10);
    if (*end || len < 1 || len > (long)sizeof data) return 2;
    int used = 0;
    while (used < len) {
        int n = lx_read(0, data + used, len - used);
        if (n <= 0) return 1;
        used += n;
    }
    return lx_save_file(argv[1], data, used) < 0;
}
