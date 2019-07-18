#include "audio.h"

extern "C" {
    #include <libavutil/avassert.h>
}

static char g_error[AV_ERROR_MAX_STRING_SIZE] = {0};

/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 */
int init_converted_audio_samples(uint8_t ***converted_input_samples,
                                 AVCodecContext *output_codec_context,
                                 int frame_size)
{
    int error;

    /**
     * Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
    if (!(*converted_input_samples = (uint8_t**) calloc(output_codec_context->channels,
                                            sizeof(**converted_input_samples)))) {
        fprintf(stderr, "Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    /**
     * Allocate memory for the samples of all channels in one consecutive
     * block for convenience.
     */
    if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                  output_codec_context->channels,
                                  frame_size,
                                  output_codec_context->sample_fmt, 0)) < 0) {
        fprintf(stderr,
                "Could not allocate converted input samples (error '%s')\n",
                av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, error));
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return error;
    }
    return 0;
}

/**
 * Initialize the audio resampler based on the input and output codec settings.
 * If the input and output sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 */
int init_audio_resampler(AVCodecContext *input_codec_context,
                         AVCodecContext *output_codec_context,
                         SwrContext **resampler_context)
{
    int error;

    /**
        * Create a resampler context for the conversion.
        * Set the conversion parameters.
        * Default channel layouts based on the number of channels
        * are assumed for simplicity (they are sometimes not detected
        * properly by the demuxer and/or decoder).
        */
    *resampler_context = swr_alloc_set_opts(NULL,
                                            av_get_default_channel_layout(output_codec_context->channels),
                                            output_codec_context->sample_fmt,
                                            output_codec_context->sample_rate,
                                            av_get_default_channel_layout(input_codec_context->channels),
                                            input_codec_context->sample_fmt,
                                            input_codec_context->sample_rate,
                                            0, NULL);
    if (!*resampler_context)
    {
        fprintf(stderr, "Could not allocate resampler context\n");
        return AVERROR(ENOMEM);
    }

    /**
    * Perform a sanity check so that the number of converted samples is
    * not greater than the number of samples to be converted.
    * If the sample rates differ, this case has to be handled differently
    */
    av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

    /** Open the resampler with the specified parameters. */
    if ((error = swr_init(*resampler_context)) < 0)
    {
        fprintf(stderr, "Could not open resample context\n");
        swr_free(resampler_context);
        return error;
    }

    return 0;
}

/** Initialize a FIFO buffer for the audio samples to be encoded. */
int init_audio_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is specified
 * by frame_size.
 */
int convert_audio_samples(const uint8_t **input_data,
                          uint8_t **converted_data, const int frame_size,
                          SwrContext *resample_context)
{
    int error;

    /** Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, frame_size,
                             input_data    , frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, error));
        return error;
    }

    return 0;
}

/** Add converted input audio samples to the FIFO buffer for later processing. */
int add_samples_to_audio_fifo(AVAudioFifo *fifo,
                              uint8_t **converted_input_samples,
                              const int frame_size)
{
    int error;

    /**
     * Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples.
     */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /** Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 */
int init_output_audio_frame(AVFrame **frame,
                            AVCodecContext *output_codec_context,
                            int frame_size)
{
    int error;

    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc()))
    {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /**
     * Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /**
     * Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified.
     */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0)
    {
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}
