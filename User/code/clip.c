#include "../gui/shared-clipboard.h"

int main(int argc, char **argv) {
    int copy = argc >= 2 && !strcmp(argv[1], "copy");
    int mac = argc == 3 && !strcmp(argv[2], "--mac");
    if ((!copy && !(argc >= 2 && !strcmp(argv[1], "paste"))) || (argc != 2 && !mac)) {
        fputs("usage: clip copy|paste [--mac]\n", stderr);
        return 2;
    }
    unsigned size = 0;
    unsigned char *data = NULL;
    if (copy || !mac) {
        FILE *in = copy ? stdin : fopen(LX_CLIP_FILE, "rb");
        data = malloc(LX_CLIP_MAX + 1);
        if (!in || !data)
            return 1;
        size = fread(data, 1, LX_CLIP_MAX + 1, in);
        if (ferror(in) || size > LX_CLIP_MAX) {
            fputs("clip: read failed or text exceeds 1 MiB\n", stderr);
            return 1;
        }
        fclose(in);
    }
    if (mac && clip_mac(copy, &data, &size) < 0) {
        fputs("clip: Mac bridge unavailable or clipboard exceeds 1 MiB\n", stderr);
        return 1;
    }
    if (copy)
        return clip_store(data, size) != 0;
    return fwrite(data, 1, size, stdout) != size || fflush(stdout) != 0;
}
