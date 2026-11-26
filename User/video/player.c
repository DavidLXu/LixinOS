#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include "../desktop/window-widgets.h"
#include "../console.h"
#include "audio-track.h"

#define DEMO "/lib/video/demo.mp4"
static const AVRational milliseconds = {1, 1000};
typedef struct {
    AVFormatContext *file;
    AudioTrack *audio;
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *frame, *next;
    int stream, draining, queued;
    int64_t origin, position, upcoming, duration, interval, anchor;
} Movie;

static Movie movie;
static struct SwsContext *scaler;
static uint8_t *surface[4];
static int strides[4], surface_w, surface_h;
static char path[513];
static const char *notice;
static int playing, ended;
static UiControls controls;
static UiText editor;
enum { PATH=1, OPEN, PLAY, SEEK };

static void close_movie(Movie *m) {
    audio_track_free(m->audio);
    av_frame_free(&m->frame);
    av_frame_free(&m->next);
    av_packet_free(&m->packet);
    avcodec_free_context(&m->decoder);
    avformat_close_input(&m->file);
    *m=(Movie){0};
}

/* Receive delayed frames too, including the last B-frames after demuxer EOF. */
static int decode(Movie *m) {
    m->queued=0;
    for (;;) {
        int r=avcodec_receive_frame(m->decoder,m->next);
        if (r==0) {
            int64_t pts=m->next->best_effort_timestamp;
            AVRational tb=m->file->streams[m->stream]->time_base;
            if (m->origin==AV_NOPTS_VALUE && pts!=AV_NOPTS_VALUE) m->origin=pts;
            m->upcoming=pts==AV_NOPTS_VALUE ? m->position+m->interval :
                av_rescale_q(pts-m->origin,tb,milliseconds);
            if (m->upcoming<0) m->upcoming=0;
            m->queued=1;
            return 1;
        }
        if (r==AVERROR_EOF) return 0;
        if (r!=AVERROR(EAGAIN) || m->draining) return -1;
        do {
            av_packet_unref(m->packet);
            r=av_read_frame(m->file,m->packet);
        } while (r>=0 && m->packet->stream_index!=m->stream);
        if (r==AVERROR_EOF) {
            m->draining=1;
            r=avcodec_send_packet(m->decoder,NULL);
        } else if (r>=0) {
            r=avcodec_send_packet(m->decoder,m->packet);
            av_packet_unref(m->packet);
        }
        if (r<0) return -1;
    }
}

static void present(Movie *m) {
    av_frame_unref(m->frame);
    av_frame_move_ref(m->frame,m->next);
    m->position=m->upcoming;
    m->queued=0;
}

static const char *load(Movie *m,const char *filename) {
    if (lx_stat(filename)<0) return "无法打开文件";
    if (avformat_open_input(&m->file,filename,NULL,NULL)<0 ||
        avformat_find_stream_info(m->file,NULL)<0) return "无法识别视频文件";
    const AVCodec *codec=NULL;
    m->stream=av_find_best_stream(m->file,AVMEDIA_TYPE_VIDEO,-1,-1,&codec,0);
    if (m->stream<0) return "没有可播放的视频轨道";
    AVStream *stream=m->file->streams[m->stream];
    m->decoder=avcodec_alloc_context3(codec);
    m->packet=av_packet_alloc();
    m->frame=av_frame_alloc();
    m->next=av_frame_alloc();
    if (!m->decoder || !m->packet || !m->frame || !m->next) return "内存不足";
    if (avcodec_parameters_to_context(m->decoder,stream->codecpar)<0) return "无法读取视频参数";
    m->decoder->thread_count=1;
    m->decoder->max_pixels=4096LL*2304;
    if (avcodec_open2(m->decoder,codec,NULL)<0) return "无法启动视频解码器";
    m->origin=stream->start_time;
    if (stream->duration>0) m->duration=av_rescale_q(stream->duration,stream->time_base,milliseconds);
    else if (m->file->duration>0) m->duration=m->file->duration/1000;
    AVRational fps=av_guess_frame_rate(m->file,stream,NULL);
    m->interval=fps.num>0 && fps.den>0 ? av_rescale_q(1,av_inv_q(fps),milliseconds) : 40;
    if (m->interval<1) m->interval=1;
    m->position=-m->interval;
    if (decode(m)!=1) return "无法解码第一帧";
    present(m);
    m->audio=audio_track_open(filename);
    return NULL;
}

static void queue_next(void) {
    if (decode(&movie)<0) { notice="视频解码失败"; playing=0; }
}

static void open_path(void) {
    Movie candidate={0};
    playing=0;
    notice=load(&candidate,path);
    if (!notice) {
        close_movie(&movie);
        movie=candidate;
        ended=0;
        playing=1;
        queue_next();
        movie.anchor=console_call(38,0)-movie.position;
    } else {
        close_movie(&candidate);
        close_movie(&movie);
        ended=0;
    }
    controls.focus=PLAY;
}

static void seek_to(int64_t target) {
    if (!movie.file) return;
    if (movie.duration>0 && target>=movie.duration) target=movie.duration-1;
    if (target<0) target=0;
    AVStream *st=movie.file->streams[movie.stream];
    int64_t ts=av_rescale_q(target,milliseconds,st->time_base);
    if (movie.origin!=AV_NOPTS_VALUE) ts+=movie.origin;
    if (av_seek_frame(movie.file,movie.stream,ts,AVSEEK_FLAG_BACKWARD)<0) {
        notice="无法跳转到该位置";
        return;
    }
    notice=NULL;
    audio_track_seek(movie.audio,target);
    avcodec_flush_buffers(movie.decoder);
    av_frame_unref(movie.next);
    movie.draining=movie.queued=ended=0;
    movie.position=target-movie.interval;
    do {
        queue_next();
        if (!movie.queued) break;
        present(&movie);
    } while (movie.position<target);
    if (!notice) queue_next();
    movie.anchor=console_call(38,0)-movie.position;
}

static void toggle(void) {
    if (!movie.file) return;
    if (ended) seek_to(0);
    if (notice) return;
    playing=!playing;
    if (!playing) audio_track_pause(movie.audio);
    else audio_track_seek(movie.audio,movie.position);
    if (playing) movie.anchor=console_call(38,0)-movie.position;
}

static AppControl control(AppWindow *s,int id) {
    switch (id) {
    case PATH: return (AppControl){id,8,5,s->width-74,26,NULL};
    case OPEN: return (AppControl){id,s->width-59,5,51,26,"打开"};
    case PLAY: return (AppControl){id,8,s->height-31,56,26,playing?"暂停":ended?"重播":"播放"};
    default: return (AppControl){id,8,s->height-47,s->width-16,14,NULL};
    }
}

static void draw(AppWindow *s) {
    app_rect(s,0,0,s->width,s->height,0x1083);
    int area=s->height-85;
    AVFrame *f=movie.frame;
    if (f && area>0 && f->width>0 && f->height>0) {
        AVRational sar=av_guess_sample_aspect_ratio(movie.file,movie.file->streams[movie.stream],f);
        double aspect=(double)f->width/f->height*(sar.num>0 && sar.den>0 ? av_q2d(sar) : 1);
        int w=s->width, h=area;
        if (area*aspect<w) w=(int)(area*aspect+0.5);
        else h=(int)(w/aspect+0.5);
        if (w<1) w=1;
        if (h<1) h=1;
        if (w!=surface_w || h!=surface_h || !surface[0]) {
            av_freep(&surface[0]);
            surface_w=w; surface_h=h;
            if (av_image_alloc(surface,strides,w,h,AV_PIX_FMT_RGB565LE,32)<0) surface[0]=NULL;
        }
        scaler=sws_getCachedContext(scaler,f->width,f->height,f->format,w,h,
                                    AV_PIX_FMT_RGB565LE,SWS_BILINEAR,NULL,NULL,NULL);
        if (surface[0] && scaler &&
            sws_scale(scaler,(const uint8_t *const *)f->data,f->linesize,0,f->height,surface,strides)==h) {
            int x=(s->width-w)/2,y=36+(area-h)/2;
            for (int i=0;i<h;++i)
                memcpy(s->pixels+(y+i)*s->width+x,surface[0]+i*strides[0],w*2);
        } else notice="无法显示视频画面";
    }
    app_field(s,control(s,PATH),&editor,path,controls.focus==PATH);
    app_button(s,&controls,control(s,OPEN),0);
    AppControl bar=control(s,SEEK);
    app_rect(s,bar.x,bar.y+5,bar.w,4,0x528a);
    if (movie.duration>0) {
        int width=ended ? bar.w : ui_limit((int)(movie.position*bar.w/movie.duration),bar.w);
        app_rect(s,bar.x,bar.y+5,width,4,0x56b4);
    }
    app_button(s,&controls,control(s,PLAY),1);
    long p=(ended && movie.duration>0 ? movie.duration : movie.position)/1000;
    long d=movie.duration/1000;
    char status[96];
    if (d>0) snprintf(status,sizeof status,"%ld:%02ld / %ld:%02ld",p/60,p%60,d/60,d%60);
    else snprintf(status,sizeof status,"%ld:%02ld",p/60,p%60);
    label(s,75,s->height-30,s->width-80,notice ? notice : status,notice ? 0xfda9 : 0xffff);
    if (s->width>320 && movie.file && !movie.audio) label(s,s->width-68,s->height-30,64,"无音轨",0xc618);
}

static int event(AppWindow *s,WindowEvent e) {
    int dirty=1;
    if (e.type==WINDOW_CLIENT_TIMER) {
        int64_t elapsed=console_call(38,0)-movie.anchor;
        dirty=0;
        if (playing) audio_track_pump(movie.audio,elapsed);
        for (int i=0;playing && movie.queued && movie.upcoming<=elapsed && i<6;++i) {
            present(&movie); queue_next(); dirty=1;
        }
        if (playing && !movie.queued && elapsed>=FFMAX(movie.position+movie.interval,movie.duration)) {
            playing=0; ended=1; dirty=1;
            audio_track_pause(movie.audio);
        }
    } else if (e.type==WINDOW_CLIENT_POINTER) {
        int hit=0;
        for (int id=PATH;id<=SEEK;++id) if (app_control_hit(control(s,id),e.x,e.y)) hit=id;
        int action=ui_pointer(&controls,hit,e.value!=0);
        if (action==PATH) ui_text_begin(&editor,path,1);
        if (action==OPEN) open_path();
        if (action==PLAY) toggle();
        if (action==SEEK && movie.duration>0) {
            AppControl bar=control(s,SEEK);
            seek_to((e.x-bar.x)*movie.duration/bar.w);
        }
    } else if (e.type==WINDOW_CLIENT_KEY) {
        if (e.value==15) { controls.focus=PATH; ui_text_begin(&editor,path,1); }
        else if (controls.focus==PATH) {
            if (e.value==13 || e.value==10) open_path();
            else if (e.value==27) controls.focus=PLAY;
            else app_field_key(s,&editor,path,sizeof path,e.value);
        } else if (e.value==' ' || e.value==13) toggle();
        else if (e.value==UI_LEFT || e.value==UI_RIGHT)
            seek_to(movie.position+(e.value==UI_LEFT ? -5000 : 5000));
    } else if (e.type==WINDOW_CLIENT_PASTE && controls.focus==PATH)
        app_field_paste(s,&editor,path,sizeof path,e.value);
    s->timer_ms=playing ? (movie.audio ? 10 : (unsigned)av_clip64(movie.interval,16,100)) : 0;
    return dirty;
}

int main(int argc,char **argv) {
    av_log_set_level(AV_LOG_QUIET);
    snprintf(path,sizeof path,"%s",argc>1 ? argv[1] : DEMO);
    ui_text_begin(&editor,path,0);
    open_path();
    int result=app_run3(event,draw);
    close_movie(&movie);
    av_freep(&surface[0]);
    sws_freeContext(scaler);
    return result;
}
