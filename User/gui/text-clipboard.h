#ifndef LIXIN_TEXT_CLIPBOARD_H
#define LIXIN_TEXT_CLIPBOARD_H

/* Options for canvas pointer polling, with a Copy event returned by the menu. */
enum { LX_CONTEXT_CUSTOM = 8, LX_CONTEXT_EDIT = 16, LX_RIGHT_PRESS = 1UL << 29 };
enum { LX_PASTE_MULTILINE = 1, LX_COPY_ENABLED = 2, LX_PASTE_BLOCK = 4, LX_POINTER_COPY = 1UL << 30 };
long lx_console_call(long operation, long value);

static inline void lx_copy_text(const char *text, unsigned length) {
    char *mail = (char *)0x443d8000UL;
    if (length > 4096) length = 4096;
    if (!length) return;
    for (unsigned i = 0; i < length; ++i) mail[i] = text[i];
    lx_console_call(34, length);
}

#endif
