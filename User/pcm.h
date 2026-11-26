#pragma once
#include "console.h"
#include "cpython/runtime.h"
#include <string.h>
/* Interleaved stereo signed 16-bit PCM at 48 kHz, up to 1024 frames per write. */
static inline long pcm_open(void) { return console_call(63, 0); }
static inline void pcm_close(void) { console_call(63, ~0UL); }
static inline long pcm_write(const short *samples, unsigned frames) {
    if (!frames || frames > 1024) return -22;
    unsigned bytes = frames * 4;
    int held = lx_runtime_lock();
    memcpy((void *)0x443d7000UL, samples, bytes);
    long result = console_call(63, bytes);
    lx_runtime_unlock(&held);
    return result;
}
