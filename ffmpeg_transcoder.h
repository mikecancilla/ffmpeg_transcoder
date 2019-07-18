#pragma once

#include <string>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavutil/audio_fifo.h>
}

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
    AVAudioFifo *audio_fifo;
} StreamContext;

typedef struct Options {
    std::string output_file;
    std::string input_file;
    std::string audio_elementary_file;
    std::string video_elementary_file;
    bool mux;
    int start_time;
    int end_time;
    bool avisynth;
    char avisynth_script[1024];
    AVRational frame_rate;
} Options;
