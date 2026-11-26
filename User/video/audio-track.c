#include "audio-track.h"
#include "../pcm.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

/* A separate demux cursor allows the audio queue to fill independently of
 * video decoding and bounds the memory used by interleaved packets. */
struct AudioTrack {
    AVFormatContext *file;
    AVCodecContext *decoder;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *swr;
    int stream, output, draining, done;
    short *buffer;
    unsigned capacity;
    int count, cursor;
    int64_t next_sample, first_sample;
};

AudioTrack *audio_track_open(const char *path) {
    AudioTrack *a = av_mallocz(sizeof(*a));
    if (!a) return NULL;
    const AVCodec *codec = NULL;
    if (avformat_open_input(&a->file,path,NULL,NULL) < 0 ||
        avformat_find_stream_info(a->file,NULL) < 0) goto fail;
    a->stream = av_find_best_stream(a->file,AVMEDIA_TYPE_AUDIO,-1,-1,&codec,0);
    if (a->stream < 0 || !(a->decoder = avcodec_alloc_context3(codec))) goto fail;
    if (avcodec_parameters_to_context(a->decoder,a->file->streams[a->stream]->codecpar) < 0 ||
        avcodec_open2(a->decoder,codec,NULL) < 0) goto fail;
    a->packet = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->packet || !a->frame) goto fail;
    return a;
fail:
    audio_track_free(a);
    return NULL;
}

void audio_track_pause(AudioTrack *a) {
    if (a && a->output) { pcm_close(); a->output = 0; }
}
void audio_track_free(AudioTrack *a) {
    if (!a) return;
    audio_track_pause(a);
    swr_free(&a->swr);
    av_free(a->buffer);
    av_packet_free(&a->packet);
    av_frame_free(&a->frame);
    avcodec_free_context(&a->decoder);
    avformat_close_input(&a->file);
    av_free(a);
}
void audio_track_seek(AudioTrack *a, long ms) {
    if (!a) return;
    audio_track_pause(a);
    int64_t origin = a->file->start_time == AV_NOPTS_VALUE ? 0 : a->file->start_time;
    if (av_seek_frame(a->file,-1,origin + ms*1000,AVSEEK_FLAG_BACKWARD) < 0) {
        a->done = 1;
        return;
    }
    avcodec_flush_buffers(a->decoder);
    swr_free(&a->swr);
    a->draining = a->done = a->count = a->cursor = 0;
    a->next_sample = ms*48;
}
static int decode_audio(AudioTrack *a) {
    int status;
    for (;;) {
        av_frame_unref(a->frame);
        status = avcodec_receive_frame(a->decoder,a->frame);
        if (status != AVERROR(EAGAIN)) break;
        if (a->draining) return 0;
        do {
            av_packet_unref(a->packet);
            status = av_read_frame(a->file,a->packet);
        } while (status >= 0 && a->packet->stream_index != a->stream);
        a->draining = status == AVERROR_EOF;
        if (status < 0 && !a->draining) return 0;
        if (avcodec_send_packet(a->decoder,a->draining ? NULL : a->packet) < 0) return 0;
    }
    if (status < 0 && status != AVERROR_EOF) return 0;
    AVFrame *f = a->frame;
    int flush = status == AVERROR_EOF;
    if (!a->swr) {
        if (flush) return 0;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&a->swr,&stereo,AV_SAMPLE_FMT_S16,48000,
                                &f->ch_layout,f->format,f->sample_rate,0,NULL) < 0 ||
            swr_init(a->swr) < 0) return 0;
    }
    int capacity = swr_get_out_samples(a->swr,flush ? 0 : f->nb_samples);
    if (capacity <= 0 || capacity > 48000*4) return 0;
    av_fast_malloc(&a->buffer,&a->capacity,(size_t)capacity*4);
    if (!a->buffer) return 0;
    a->first_sample = a->next_sample;
    if (!flush && f->best_effort_timestamp != AV_NOPTS_VALUE) {
        AVRational tb = a->file->streams[a->stream]->time_base;
        int64_t origin = a->file->start_time == AV_NOPTS_VALUE ? 0 : a->file->start_time;
        a->first_sample = av_rescale_q(f->best_effort_timestamp,tb,(AVRational){1,48000})
                          - av_rescale(origin,48000,AV_TIME_BASE)
                          - swr_get_delay(a->swr,48000);
    }
    uint8_t *out = (uint8_t *)a->buffer;
    a->count = swr_convert(a->swr,&out,capacity,flush ? NULL : (const uint8_t **)f->extended_data,
                           flush ? 0 : f->nb_samples);
    a->cursor = 0;
    if (a->count < 0) return 0;
    a->next_sample = a->first_sample + a->count;
    return !flush || a->count > 0;
}
void audio_track_pump(AudioTrack *a, long ms) {
    if (!a || a->done) return;
    if (!a->output) {
        if (pcm_open() < 0) return;
        a->output = 1;
    }
    int64_t now = ms*48;
    for (int pass=0; pass<24; ++pass) {
        if (a->cursor == a->count) {
            if (!decode_audio(a)) { a->done=1; return; }
            if (!a->count) continue;
        }
        int64_t start = a->first_sample + a->cursor;
        if (start > now + 1920) return;
        if (start + 2400 < now) {
            a->cursor += FFMIN((int64_t)(a->count-a->cursor),now-start);
            continue;
        }
        int frames = FFMIN(a->count-a->cursor,1024);
        if (pcm_write(a->buffer+2*a->cursor,frames) < 0) return;
        a->cursor += frames;
    }
}
