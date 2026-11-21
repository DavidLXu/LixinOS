#pragma once
#include <stdio.h>
enum { COUNT = 20 };
static const char *const labels[COUNT] = {
    "AC", "+/-", "DEL", "/", "7", "8", "9", "*", "4", "5", "6", "-",
    "1", "2", "3", "+", "0", ".", "=", "Close"
};
static const int keys[COUNT] = {
    'c', 's', 127, '/', '7', '8', '9', '*', '4', '5', '6', '-',
    '1', '2', '3', '+', '0', '.', '=', 'q'
};
static char input[20] = "0";
static double accumulator;
static int operation, fresh = 1, error;

static double value(void) {
    const char *p = input;
    int negative = *p == '-';
    p += negative;
    double number = 0, scale = 0;
    while (*p) {
        if (*p == '.') scale = 1;
        else if (scale) { scale *= 10; number += (*p - '0') / scale; }
        else number = number * 10 + (*p - '0');
        ++p;
    }
    return negative ? -number : number;
}

static void clear(void) {
    input[0] = '0'; input[1] = 0;
    fresh = 1; error = operation = 0; accumulator = 0;
}

static void press(int key) {
    unsigned n = 0;
    while (input[n]) ++n;
    if (key == 'c') { clear(); return; }
    if (error) return;
    if ((key >= '0' && key <= '9') || key == '.') {
        if (fresh || (n == 1 && input[0] == '0' && key != '.')) {
            n = 0; input[0] = 0;
        }
        if (key == '.') {
            for (unsigned i = 0; i < n; ++i) if (input[i] == '.') return;
            if (!n) input[n++] = '0';
        }
        if (n < 15) { input[n++] = key; input[n] = 0; }
        fresh = 0;
    } else if ((key == 8 || key == 127) && !fresh) {
        if (n > 1 && !(n == 2 && input[0] == '-')) input[n - 1] = 0;
        else { input[0] = '0'; input[1] = 0; }
    } else if (key == 's') {
        if (input[0] == '-') {
            for (unsigned i = 0; i < n; ++i) input[i] = input[i + 1];
        } else if (n < 16 && value() != 0) {
            for (unsigned i = n + 1; i; --i) input[i] = input[i - 1];
            input[0] = '-';
        }
        fresh = 0;
    } else if (key == '+' || key == '-' || key == '*' || key == '/' || key == '=') {
        double rhs = value();
        if (operation && !fresh) {
            switch (operation) {
            case '+': accumulator += rhs; break;
            case '-': accumulator -= rhs; break;
            case '*': accumulator *= rhs; break;
            case '/': if (rhs == 0) { error = 1; return; } accumulator /= rhs; break;
            }
        } else if (!operation) accumulator = rhs;
        if (accumulator < -9999999999999.0 || accumulator > 9999999999999.0) {
            error = 2; return;
        }
        int len = snprintf(input, sizeof(input), "%.8f", accumulator);
        if (len < 0 || len >= (int)sizeof(input)) {
            snprintf(input, sizeof(input), "%.0f", accumulator);
        } else {
            while (len && input[len - 1] == '0') --len;
            if (len && input[len - 1] == '.') --len;
            input[len] = 0;
        }
        operation = key == '=' ? 0 : key;
        fresh = 1;
    }
}

