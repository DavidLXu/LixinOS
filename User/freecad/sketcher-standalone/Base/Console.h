#pragma once
#include <cstdio>
namespace Base {
struct SketcherConsole {
    void log(const char *text) const { std::fputs(text, stderr); }
    template <class... Args> void log(const char *fmt, Args... args) const {
        std::fprintf(stderr, fmt, args...);
    }
    void warning(const char *text) const { log(text); }
};
inline SketcherConsole Console() { return {}; }
}
