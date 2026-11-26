#include <gnu/libc-version.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("Hello from glibc %s: %s\n", gnu_get_libc_version(), argc > 1 ? argv[1] : "LixinOS");
    return argc > 2 && strcmp(argv[2], "status-test") == 0 ? 37 : 0;
}
