#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "../pcm.h"

enum { VOICES = 32, FRAMES = 480 };
static struct voice {
    const unsigned char *data;
    unsigned length, pos, rate, fraction, left, right;
    int lump;
} voices[VOICES];
static short mix[FRAMES * 2];
static int prefix, pending;

static boolean init(boolean use_prefix) {
    prefix = use_prefix;
    return pcm_open() == 0;
}
static int lump_number(sfxinfo_t *sfx) {
    char name[12];
    if (sfx->link) sfx = sfx->link;
    snprintf(name, sizeof(name), "%s%.8s", prefix ? "DS" : "", sfx->name);
    return W_CheckNumForName(name);
}
static void stop(int channel) {
    if ((unsigned)channel >= VOICES || !voices[channel].data) return;
    int lump = voices[channel].lump;
    voices[channel].data = NULL;
    for (unsigned i=0; i<VOICES; ++i)
        if (voices[i].data && voices[i].lump == lump) return;
    W_ReleaseLumpNum(lump);
}
static void adjust(int channel, int volume, int separation) {
    if ((unsigned)channel >= VOICES) return;
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    if (separation < 0) separation = 0;
    if (separation > 255) separation = 255;
    voices[channel].left = volume * (255 - separation) / 127;
    voices[channel].right = volume * separation / 127;
}
static int start(sfxinfo_t *sfx, int channel, int volume, int separation) {
    int lump = lump_number(sfx);
    if ((unsigned)channel >= VOICES || lump < 0) return -1;
    stop(channel);
    int bytes = W_LumpLength(lump);
    if (bytes < 9) return -1;
    const uint8_t *p = W_CacheLumpNum(lump, PU_STATIC);
    uint32_t size = p[4] | (uint32_t)p[5]<<8 | (uint32_t)p[6]<<16 | (uint32_t)p[7]<<24;
    unsigned rate = p[2] | p[3]<<8;
    voices[channel] = (struct voice){.lump=lump, .data=p+8, .length=size, .rate=rate};
    if (p[0] != 3 || p[1] || !rate || !size || size > (unsigned)bytes-8) {
        stop(channel);
        return -1;
    }
    adjust(channel,volume,separation);
    return channel;
}
static short clip(int value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}
static void update(void) {
    for (unsigned block=0; block<6; ++block) {
        if (!pending) {
            int active = 0;
            for (unsigned i=0; i<VOICES; ++i) active |= voices[i].data != NULL;
            if (!active) break;
            for (unsigned frame=0; frame<FRAMES; ++frame) {
                int left=0, right=0;
                for (unsigned i=0; i<VOICES; ++i) {
                    struct voice *v = &voices[i];
                    if (!v->data) continue;
                    int sample = (int)v->data[v->pos] - 128;
                    left += sample * (int)v->left;
                    right += sample * (int)v->right;
                    v->fraction += v->rate;
                    v->pos += v->fraction / 48000;
                    v->fraction %= 48000;
                    if (v->pos >= v->length) stop(i);
                }
                mix[frame*2] = clip(left);
                mix[frame*2+1] = clip(right);
            }
            pending = 1;
        }
        if (pcm_write(mix,FRAMES) < 0) break;
        pending = 0;
    }
}
static boolean playing(int channel) {
    return (unsigned)channel < VOICES && voices[channel].data;
}
static void shutdown(void) {
    for (unsigned i=0; i<VOICES; ++i) stop(i);
    pcm_close();
    pending = 0;
}
static snddevice_t devices[] = {SNDDEVICE_SB};
sound_module_t DG_sound_module = {
    .sound_devices=devices, .num_sound_devices=1, .Init=init, .Shutdown=shutdown,
    .GetSfxLumpNum=lump_number, .Update=update, .UpdateSoundParams=adjust,
    .StartSound=start, .StopSound=stop, .SoundIsPlaying=playing,
};
