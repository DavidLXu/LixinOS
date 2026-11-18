#include <stdio.h>

int main(void) {
    const char *prompt = "C echo (one line)> ";
    while (*prompt) putchar(*prompt++);
    int c;
    while ((c = getchar()) != EOF && c != '\r' && c != '\n' && c != 4)
        putchar(c);
    putchar('\n');
    return 0;
}
