#pragma once

#include <stdint.h>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
    #include <libavutil/audio_fifo.h>
}

int init_converted_audio_samples(uint8_t ***converted_input_samples,
                           AVCodecContext *output_codec_context,
                           int frame_size);

int init_audio_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context);

int init_audio_resampler(AVCodecContext *input_codec_context,
                   AVCodecContext *output_codec_context,
                   SwrContext **resampler_context);

int convert_audio_samples(const uint8_t **input_data,
                    uint8_t **converted_data, const int frame_size,
                    SwrContext *resample_context);

/** Add converted input audio samples to the FIFO buffer for later processing. */
int add_samples_to_audio_fifo(AVAudioFifo *fifo,
                              uint8_t **converted_input_samples,
                              const int frame_size);

int init_output_audio_frame(AVFrame **frame,
                            AVCodecContext *output_codec_context,
                            int frame_size);
