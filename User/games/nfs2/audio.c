#include "../../pcm.h"
#include <stdint.h>

/* DirectSound mixes stereo S16 at 22050 Hz. Keep the fractional sample
   position across callbacks when feeding the 48 kHz virtio PCM device. */
void nfs_audio(const int16_t *in, unsigned frames) {
    static int available;
    static unsigned phase;
    int16_t out[1024 * 2];
    if (!available) available = pcm_open() == 0 ? 1 : -1;
    if (available < 0) return;
    unsigned count = 0;
    for (unsigned i = 0; i < frames; ++i) {
        phase += 48000;
        while (phase >= 22050) {
            phase -= 22050;
            out[count*2] = in[i*2];
            out[count*2+1] = in[i*2+1];
            if (++count == 1024) {
                pcm_write(out, count);
                count = 0;
            }
        }
    }
    if (count) pcm_write(out, count);
}
