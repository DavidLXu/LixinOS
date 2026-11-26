#pragma once
typedef struct AudioTrack AudioTrack;
AudioTrack *audio_track_open(const char *path);
void audio_track_free(AudioTrack *);
void audio_track_seek(AudioTrack *, long position_ms);
void audio_track_pump(AudioTrack *, long position_ms);
void audio_track_pause(AudioTrack *);
