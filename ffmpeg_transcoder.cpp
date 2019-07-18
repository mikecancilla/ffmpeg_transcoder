/*
* Copyright (c) 2010 Nicolas George
* Copyright (c) 2011 Stefano Sabatini
* Copyright (c) 2014 Andrey Utkin
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/

/**
* @file
* API example for demuxing, decoding, filtering, encoding and writing
* @example ffmpeg_transcoder.cpp
*/

/*
+-------------------+
|                   |
|    Main Thread    |
|                   |
|       Demux       |
|                   |
+---------+---------+
          |
          |
          | decode_input_queue
          |
          V
+-------------------+
|                   |
|   Decode Thread   |
|                   |
|       Decode      |
|                   |
+---------+---------+
          |
          |
          | filter_convert_encode_write_input_queue
          |
          V
+-------------------+
|                   |
| Filter, Convert,  |
|  Encode, Write    |
|      Thread       |
|                   |
|  Filter, Convert, |
|   Encode, Write   |
+-------------------+
*/

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/common.h>
    #include <libavutil/opt.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/threadmessage.h>
    #include <libavutil/time.h>
    #include <libavutil/audio_fifo.h>
    #include <libswresample/swresample.h>
}

#include "ffmpeg_transcoder.h"
#include "fr_conversion.h"
#include "filters.h"
#include "utils.h"

#define USE_FILTER_GRAPH 1
#define VIDEO_ELEMENTARY_NAME _T("video")
#define VIDEO_ELEMENTARY_EXT _T("h264")
#define AUDIO_ELEMENTARY_NAME _T("audio")
#define AUDIO_ELEMENTARY_EXT _T(".aac")

#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <iostream>
#include <locale> // time functions

#ifdef WINDOWS
#define WORK_DIR _T("work\\")  // work dir relative to program working directory
#else
#define WORK_DIR _T("/work/")  // work dir a volume of its own on Linux
#endif

// For use on WINDOWS when trying to chase down memory leaks
//#include <vld.h>

// Audio helper functions
#include "audio.h"

// Write frame to disk helper
#include "write_frame.h"

// Types
////////

typedef struct FrameAndStream {
    AVFrame *frame;
    int     stream_index;
} FrameAndStream;

// Defines
//////////

#define THREAD_QUEUE_SIZE 8
#define OUTPUT_AUDIO_BIT_RATE 96000

// Public Globals
/////////////////

// Options
Options             g_options;

// Describes the input file
AVFormatContext     *g_ifmt_ctx = NULL;
FilteringContext    *g_filter_ctx = NULL;
StreamContext       *g_stream_ctx = NULL;

// Private Globals
//////////////////

// FILTER, CONVERT, ENCODE, WRITE INPUT QUEUE
static AVThreadMessageQueue *g_filter_convert_encode_write_input_queue = NULL;

// DECODE INPUT QUEUE
static AVThreadMessageQueue *g_decode_input_queue = NULL;

static char g_error[AV_ERROR_MAX_STRING_SIZE] = {0};

// One entry per output file
static std::vector<AVFormatContext *> g_output_formats;

static SwrContext *g_resampler_context = NULL;

static ContinueMutex g_continue_audio_mutex;
static ContinueMutex g_continue_video_mutex;

static unsigned g_flags = 0; //AV_THREAD_MESSAGE_NONBLOCK;
static unsigned g_video_frame_num = 0;
static unsigned g_audio_frame_num = 0;
static double g_total_frames = 0;
static double g_total_duration = 0;
static uint32_t g_percentage = (uint32_t) -1;

// Forward declarations
///////////////////////

static int filter_convert_encode_write_frame(AVFrame *frame, unsigned int stream_index);
static void *filter_convert_encode_write_thread_proc(void *arg);
static int convert_encode_write_frame(AVFrame *frame, unsigned int stream_index, int *got_frame);
static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame);

///////////////////////////////////////////////////////////////////////////////
// DECODE THREAD PROC - reads from decode_input_queue
///////////////////////////////////////////////////////////////////////////////
static void *decode_thread_proc(void *arg)
{
    // Create the filter_encode_write_input_queue, will contain decoded frames
    int ret = av_thread_message_queue_alloc(&g_filter_convert_encode_write_input_queue, THREAD_QUEUE_SIZE, sizeof(FrameAndStream));

    if (ret < 0)
        return NULL;

    // Create the filter_encode_write_thread
    pthread_t filter_encode_write_thread;

    if ((ret = pthread_create(&filter_encode_write_thread, NULL, filter_convert_encode_write_thread_proc, NULL)))
    {
        av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));

        av_thread_message_queue_free(&g_filter_convert_encode_write_input_queue);

        return NULL;
    }

    // Recieve this, av_thread_message_queue_recv copies into this
    AVPacket packet;

    while(g_continue_audio_mutex.get() ||
          g_continue_video_mutex.get())
    {
        // Get a packet off the decode_input_queue
        ret = av_thread_message_queue_recv(g_decode_input_queue, &packet, g_flags);

        if (ret == AVERROR(EAGAIN))
        {
            av_usleep(10000);
            continue;
        }

        if (ret < 0)
        {
            // Put the error on the downstream queue so the downstream thread knows to exit
            av_thread_message_queue_set_err_recv(g_filter_convert_encode_write_input_queue, ret);
            break;
        }

        int stream_index = packet.stream_index;

        AVMediaType type = g_ifmt_ctx->streams[packet.stream_index]->codecpar->codec_type;

        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n", stream_index);

        if (g_ifmt_ctx->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
            g_ifmt_ctx->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");

            //av_packet_rescale_ts(&packet,
            //                     g_ifmt_ctx->streams[stream_index]->time_base,
            //                     g_stream_ctx[stream_index].dec_ctx->time_base);

            // Send a packet to the decoder
            ret = avcodec_send_packet(g_stream_ctx[stream_index].dec_ctx, &packet);

            // Unref the packet
            av_packet_unref(&packet);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0)
            {
                FrameAndStream frame_and_stream;

                frame_and_stream.frame = av_frame_alloc();

                if (!frame_and_stream.frame)
                {
                    av_log(NULL, AV_LOG_ERROR, "Decode thread could not allocate frame\n");
                    ret = AVERROR(ENOMEM);
                    break;
                }

                // Get a frame from the decoder
                ret = avcodec_receive_frame(g_stream_ctx[stream_index].dec_ctx, frame_and_stream.frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    av_frame_free(&frame_and_stream.frame);
                    break;
                }
                else if (ret < 0)
                {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    av_frame_free(&frame_and_stream.frame);
                    break;
                }

                if (ret >= 0)
                {
                    bool bWriteFrameToDisk = false;

                    // Optionally write frame to disk
                    if(type == AVMEDIA_TYPE_VIDEO &&
                       bWriteFrameToDisk)
                    {
                        static int i = 0;

                        // Save the frame to disk
                        if(i++ < 100)
                            WriteFrame(g_stream_ctx[stream_index].dec_ctx, frame_and_stream.frame, i);
                    }

                    frame_and_stream.stream_index = stream_index;

                    // Put frame on filter_encode_write_input_queue
                    ret = av_thread_message_queue_send(g_filter_convert_encode_write_input_queue, &frame_and_stream, g_flags);

                    if (g_flags && ret == AVERROR(EAGAIN))
                    {
                        ret = av_thread_message_queue_send(g_filter_convert_encode_write_input_queue, &frame_and_stream, 0);
                        av_log(g_ifmt_ctx, AV_LOG_WARNING,
                               "filter_encode_write_input_queue message queue blocking; consider raising the "
                               "thread_queue_size option (current value: %d)\n",
                               THREAD_QUEUE_SIZE);
                    }

                    if (ret < 0)
                    {
                        if (ret != AVERROR_EOF)
                            av_log(g_ifmt_ctx, AV_LOG_ERROR,
                                   "Unable to send packet to filter_encode_write_input_queue: %s\n",
                                   "error");//av_err2str(ret));

                        av_frame_free(&frame_and_stream.frame);

                        av_thread_message_queue_set_err_recv(g_filter_convert_encode_write_input_queue, ret);
                        break;
                    }
                }
            }
        }
        else
        {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(&packet,
                                 g_ifmt_ctx->streams[stream_index]->time_base,
                                 g_output_formats[stream_index]->streams[0]->time_base);
        
            //ret = av_interleaved_write_frame(g_output_formats[stream_index], &packet);  // Use if muxing
            ret = av_write_frame(g_output_formats[stream_index], &packet); // Use if writing elementary stream
        
            av_packet_unref(&packet);
        
            if (ret < 0)
                break;
        }
    }

    g_continue_audio_mutex.set(false);
    g_continue_video_mutex.set(false);

    // HERE FOR REFERENCE
    // Use the following kind of cleanup only for a drastic error where we dont
    //  exit by sending an EOS or error to the downstream queue.
    //FrameAndStream frame_and_stream = {0};
    //av_thread_message_queue_set_err_send(filter_encode_write_input_queue, AVERROR_EOF);
    //while (av_thread_message_queue_recv(filter_encode_write_input_queue, &frame_and_stream, 0) >= 0)
    //    av_frame_unref(frame_and_stream.frame);

    // Wait for filter_encode_write_thread to exit
    pthread_join(filter_encode_write_thread, NULL);

    av_thread_message_flush(g_decode_input_queue);

    av_thread_message_queue_free(&g_filter_convert_encode_write_input_queue);

    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// FILTER, CONVERT, ENCODE and WRITE thread proc - reads from filter_encode_write_input_queue
///////////////////////////////////////////////////////////////////////////////
static void *filter_convert_encode_write_thread_proc(void *arg)
{
    FrameAndStream frame_and_stream;
    int ret = 0;

    while(g_continue_audio_mutex.get() ||
          g_continue_video_mutex.get())
    {
        // Get a frame off the filter_convert_encode_write_input_queue
        ret = av_thread_message_queue_recv(g_filter_convert_encode_write_input_queue, &frame_and_stream, g_flags);

        if (ret == AVERROR(EAGAIN))
        {
            av_usleep(10000);
            continue;
        }

        if(ret < 0)
            break;

        frame_and_stream.frame->pts = frame_and_stream.frame->best_effort_timestamp;

        // Filter frame, convert, encode, and write it to disk
#if USE_FILTER_GRAPH
        ret = filter_convert_encode_write_frame(frame_and_stream.frame, frame_and_stream.stream_index);
        av_frame_free(&frame_and_stream.frame);
#else
        ret = convert_encode_write_frame(frame_and_stream.frame, frame_and_stream.stream_index, NULL);
#endif

        if (ret < 0)
            break;
    }

    g_continue_audio_mutex.set(false);
    g_continue_video_mutex.set(false);

    av_thread_message_flush(g_filter_convert_encode_write_input_queue);

    return NULL;
}

static int open_input_file(const std::string &inFileName)
{
    int ret;
    unsigned int i;
    g_ifmt_ctx = NULL;

    if ((ret = avformat_open_input(&g_ifmt_ctx, inFileName.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file: %s\n", av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, ret));
        return ret;
    }

    if ((ret = avformat_find_stream_info(g_ifmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    g_stream_ctx = (StreamContext *)av_mallocz_array(g_ifmt_ctx->nb_streams, sizeof(*g_stream_ctx));
    if (!g_stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < g_ifmt_ctx->nb_streams; i++)
    {
        AVStream *stream = g_ifmt_ctx->streams[i];

        AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx = NULL;

        if (!dec)
            continue;

        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }

        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                "for stream #%u\n", i);
            return ret;
        }

        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
            || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                codec_ctx->framerate = av_guess_frame_rate(g_ifmt_ctx, stream, NULL);

                if(g_options.start_time == -1 &&
                   g_options.end_time == -1)
                    g_total_duration = (double) g_ifmt_ctx->duration / AV_TIME_BASE;
                else
                {
                    double start_time = g_options.start_time == -1 ? 0 : g_options.start_time;
                    double end_time = g_options.end_time == -1 ? ((double) g_ifmt_ctx->duration / AV_TIME_BASE) : g_options.end_time;
                    g_total_duration = end_time - start_time;
                }

                double fps = (double) stream->avg_frame_rate.num / stream->avg_frame_rate.den;
                g_total_frames = g_total_duration / (1.0 / fps);
            }

			// Just for debugging, two fields which state frame rate
            //double frame_rate = stream->r_frame_rate.num / (double)stream->r_frame_rate.den;
            //frame_rate = stream->avg_frame_rate.num / (double)stream->avg_frame_rate.den;

            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }

        g_stream_ctx[i].dec_ctx = codec_ctx;
    }

    av_dump_format(g_ifmt_ctx, 0, inFileName.c_str(), 0);
    return 0;
}

static int open_output_files()
{
    AVStream *out_stream = NULL;
    AVStream *in_stream = NULL;
    AVCodec *encoder = NULL;
    int ret;
    unsigned int i;
	AVFormatContext *ofmt_ctx = NULL;

    for (i = 0; i < g_ifmt_ctx->nb_streams; i++)
    {
        if(NULL == g_stream_ctx[i].dec_ctx)
            continue;

        in_stream = g_ifmt_ctx->streams[i];
        AVCodecContext *dec_ctx = g_stream_ctx[i].dec_ctx;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
            dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if(dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
                //encoder = avcodec_find_encoder_by_name("libx264");
                //encoder = avcodec_find_encoder_by_name("libvpx");
                //encoder = avcodec_find_encoder_by_name("jpeg2000");
                //encoder = avcodec_find_encoder(AV_CODEC_ID_WMV2);
                //encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);
            else
                encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);

            if (!encoder)
            {
                av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found: %s\n", av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, ret));
                return AVERROR_INVALIDDATA;
            }

            AVCodecContext *enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx)
            {
                av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            std::string outFileName;

            /* In this example, we transcode to same properties (picture size,
            * sample rate etc.). These properties can be changed for output
            * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                // TODO: Just use src WxH or use command line?
                enc_ctx->width = dec_ctx->width;
                enc_ctx->height = dec_ctx->height;

                // For now, assuming 1:1 aspect ratio in target output
                enc_ctx->sample_aspect_ratio.num = 1;
                enc_ctx->sample_aspect_ratio.den = 1;

                // Check to see if decoded pixel format is supported by encoder
                enc_ctx->pix_fmt = AV_PIX_FMT_NONE;
                for (int index = 0; encoder->pix_fmts && encoder->pix_fmts[index] != -1; index++)
                {
                    if (encoder->pix_fmts[index] == dec_ctx->pix_fmt)
                    {
                        enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                        break;
                    }
                }

                // TODO: If pixel format is not supported by encoder, need to add a colorspace conversion ???
                if (enc_ctx->pix_fmt == AV_PIX_FMT_NONE)
                {
                    av_log(NULL, AV_LOG_FATAL, "Decoded colorspace not supported by encoder\n");
                    return AVERROR_INVALIDDATA;
                }

                // For now, just pass-thru framerate
                enc_ctx->time_base = av_inv_q(g_options.frame_rate);

                outFileName = g_options.video_elementary_file;
            }
            else
            {
                enc_ctx->sample_rate = dec_ctx->sample_rate; // TODO: We need flexibility here
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                enc_ctx->bit_rate = OUTPUT_AUDIO_BIT_RATE;

                /* Take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = { 1, enc_ctx->sample_rate };

                /** Allow the use of the experimental AAC encoder */
                enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

                outFileName = g_options.audio_elementary_file;
            }

			avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, outFileName.c_str());
			if (!ofmt_ctx)
			{
				av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
				return AVERROR_UNKNOWN;
			}

			/*
             * Some container formats (like MP4) require global headers to be present
             * Mark the encoder so that it behaves accordingly.
             */
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            // Use this to open with a dictionary
            //AVDictionary *dict = NULL;
            //av_dict_set(&dict, "hwaccel", "none", 0);
            //ret = avcodec_open2(enc_ctx, encoder, &dict);

            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u: %s\n", i, av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, ret));
                return ret;
            }

	        out_stream = avformat_new_stream(ofmt_ctx, NULL);
			if (!out_stream)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
				return AVERROR_UNKNOWN;
			}

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }

            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            out_stream->time_base = enc_ctx->time_base;
            g_stream_ctx[i].enc_ctx = enc_ctx;

            av_dump_format(ofmt_ctx, 0, outFileName.c_str(), 1);
			if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
			{
				ret = avio_open(&ofmt_ctx->pb, outFileName.c_str(), AVIO_FLAG_WRITE);
				if (ret < 0)
				{
					av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", outFileName);
					return ret;
				}
			}

            ret = avformat_write_header(ofmt_ctx, NULL);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
                return ret;
            }

            g_output_formats.push_back(ofmt_ctx);
        }
        else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN)
        {
            av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        }
        else
        {
            /* if this stream must be remuxed */
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
                return ret;
            }

            out_stream->time_base = in_stream->time_base;
        }
    }

    return 0;
}

static int convert_audio_frame_to_fifo(AVFrame *frame, unsigned int stream_index)
{
        /** Temporary storage for the converted input samples. */
        uint8_t **converted_input_samples = NULL;

        /** Initialize the temporary storage for the converted input samples. */
    int ret = init_converted_audio_samples(&converted_input_samples,
                                           g_stream_ctx[stream_index].enc_ctx,
                                           frame->nb_samples);
        if(ret < 0)
            return ret;

        /**
         * Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples.
         */
        ret = convert_audio_samples((const uint8_t**) frame->extended_data,
                                    converted_input_samples,
                                    frame->nb_samples,
                                    g_resampler_context);
        if(ret < 0)
            return ret;

        /** Add the converted input samples to the FIFO buffer for later processing. */
        ret = add_samples_to_audio_fifo(g_stream_ctx[stream_index].audio_fifo,
                                        converted_input_samples,
                                        frame->nb_samples);

        if(ret < 0)
            return ret;

    if (converted_input_samples)
    {
            av_freep(&converted_input_samples[0]);
            free(converted_input_samples);
        }

    return ret;
}

static int read_audio_frame_from_fifo(AVFrame **frame, unsigned int stream_index)
{
        /**
         * Use the maximum number of possible samples per frame.
         * If there is less than the maximum possible frame size in the FIFO
         * buffer use this number. Otherwise, use the maximum possible frame size
         */
        const int frame_size = FFMIN(av_audio_fifo_size(g_stream_ctx[stream_index].audio_fifo),
                                     g_stream_ctx[stream_index].enc_ctx->frame_size);

    if(frame_size)
    {
        /** Initialize temporary storage for one output frame. */
        if (init_output_audio_frame(frame, g_stream_ctx[stream_index].enc_ctx, frame_size))
            return AVERROR_EXIT;

        /**
         * Read as many samples from the FIFO buffer as required to fill the frame.
         * The samples are stored in the frame temporarily.
         */
        if (av_audio_fifo_read(g_stream_ctx[stream_index].audio_fifo, (void **) (*frame)->data, frame_size) < 0) {
            fprintf(stderr, "Could not read data from FIFO\n");
            av_frame_free(frame);
            return AVERROR_EXIT;
        }

        //av_log(NULL, AV_LOG_INFO, "av_audio_fifo_size: %d\n", av_audio_fifo_size(g_stream_ctx[stream_index].audio_fifo));
    }

    return frame_size;
}

static int encode_write_frame(AVFrame *frame, unsigned int stream_index, int *got_frame)
{
    if(got_frame)
        *got_frame = 0;

    if(AVMEDIA_TYPE_AUDIO == g_stream_ctx[stream_index].enc_ctx->codec_type)
        av_log(NULL, AV_LOG_INFO, "Encoding audio frame: %d\n", g_audio_frame_num++);
    else
    {
        uint32_t percentage = (uint32_t)(100 * ((double) g_video_frame_num++ / g_total_frames));

        if(g_percentage != percentage)
        {
            av_log(NULL, AV_LOG_INFO, "Encoding video frame: %d, %d%%\n", g_video_frame_num, percentage);
            g_percentage = percentage;
        }
    }

    // Send a frame to the encoder
    int ret = avcodec_send_frame(g_stream_ctx[stream_index].enc_ctx, frame);

    if (ret < 0)
    {
        fprintf(stderr, "Error sending a frame for encoding: %s\n", av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, ret));
        return ret;
    }

    // Retrieve any available packets from the encoder
    while (ret >= 0)
    {
        AVPacket enc_pkt = {0};

        av_init_packet(&enc_pkt);

        /* Set the packet data and size so that it is recognized as being empty. */
        enc_pkt.data = NULL;
        enc_pkt.size = 0;

        // Read a packet from the encoder
        ret = avcodec_receive_packet(g_stream_ctx[stream_index].enc_ctx, &enc_pkt);

        if (ret == AVERROR(EAGAIN))
        {
            av_packet_unref(&enc_pkt);
            return 0;
        }

        if (ret == AVERROR_EOF)
        {
            av_packet_unref(&enc_pkt);
            return ret;
        }

        if (ret < 0)
        {
            av_packet_unref(&enc_pkt);
            fprintf(stderr, "Error during encoding\n");
            return ret;
        }

        av_log(NULL, AV_LOG_DEBUG, "Writing encoded packet to elementary stream file\n");

        /* write encoded packet to its elementary stream */
        if (g_stream_ctx[stream_index].enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            av_packet_rescale_ts(&enc_pkt,
                g_ifmt_ctx->streams[stream_index]->time_base,
                g_output_formats[stream_index]->streams[0]->time_base);

            ret = av_write_frame(g_output_formats[stream_index], &enc_pkt);
            //avio_write(g_output_formats[stream_index]->pb, enc_pkt.data, enc_pkt.size);
            //ret = av_interleaved_write_frame(g_output_formats[stream_index], &enc_pkt);
            if (ret < 0)
            {
                fprintf(stderr, "Could not write audio frame packet\n");
            }
        }
        else
        {            
            avio_write(g_output_formats[stream_index]->pb, enc_pkt.data, enc_pkt.size);
            //ret = av_write_frame(g_output_formats[stream_index], &enc_pkt);
            if (ret < 0)
            {
                fprintf(stderr, "Could not write video frame packet\n");
            }
        }

        av_packet_unref(&enc_pkt);

        if (got_frame)
            *got_frame = 1;
    }

    return ret;
}

static int convert_encode_write_frame(AVFrame *frame, unsigned int stream_index, int *got_frame)
{
    int ret = 0;
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *enc_frame = frame;

    // Convert audio samples if this frame contains audio
    if(AV_PICTURE_TYPE_NONE == frame->pict_type)
    {
        ret = convert_audio_frame_to_fifo(frame, stream_index);
        if(ret < 0)
            return ret;

        // Encode an encoder context audio frame size at a time.
        // Only the final write should not be an integral encoder context
        // audio frame size.
        while(av_audio_fifo_size(g_stream_ctx[stream_index].audio_fifo) >=
              g_stream_ctx[stream_index].enc_ctx->frame_size)
        {
            AVFrame *audio_frame = NULL;

            ret = read_audio_frame_from_fifo(&audio_frame, stream_index);
            if(ret <= 0)
                return ret;

            ret = encode_write_frame(audio_frame, stream_index, got_frame);

            if (audio_frame)
                av_frame_free(&audio_frame);

            if(ret < 0)
                return ret;
        }
    }
    else
    {
        ret = encode_write_frame(enc_frame, stream_index, got_frame);
    }

    if(frame)
        av_frame_free(&frame);

    return ret;
}

static int filter_convert_encode_write_frame(AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame = NULL;

    //av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");

    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(g_filter_ctx[stream_index].buffersrc_ctx, frame, 0);

    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered frames from the filtergraph */
    while (g_continue_audio_mutex.get() ||
           g_continue_video_mutex.get())
    {
        filt_frame = av_frame_alloc();

        if (!filt_frame)
        {
            ret = AVERROR(ENOMEM);
            break;
        }

        //av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");

        ret = av_buffersink_get_frame_flags(g_filter_ctx[stream_index].buffersink_ctx, filt_frame, 0);

        if (ret < 0)
        {
            /* if no more frames for output - returns AVERROR(EAGAIN)
            *  if flushed and no more frames for output - returns AVERROR_EOF
            *  rewrite retcode to 0 to show it as normal procedure completion
            */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // Filtering can make us return early, like if we use a trim filter.
                // In this case also set the g_continue bool to false
                if (ret == AVERROR_EOF)
                {
                    if(AVMEDIA_TYPE_AUDIO == g_stream_ctx[stream_index].enc_ctx->codec_type)
                        g_continue_audio_mutex.set(false);
                    else
                        g_continue_video_mutex.set(false);
                }

                ret = 0;
            }

            av_frame_free(&filt_frame);

            break;
        }

        ret = convert_encode_write_frame(filt_frame, stream_index, NULL);

        if (ret < 0)
            break;
    }

    return ret;
}

static int flush_encoder(unsigned int stream_index)
{
    int ret;
    int got_frame = 0;

    if (!(g_stream_ctx[stream_index].enc_ctx->codec->capabilities & AV_CODEC_CAP_DELAY))
        return 0;

    av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);

    while (1)
    {
        AVFrame *enc_frame = NULL;

        if(AVMEDIA_TYPE_AUDIO == g_stream_ctx[stream_index].enc_ctx->codec_type)
        {
            ret = read_audio_frame_from_fifo(&enc_frame, stream_index);

            if(ret < 0)
                return ret;
        }

        ret = encode_write_frame(enc_frame, stream_index, &got_frame);

        if(enc_frame)
            av_frame_free(&enc_frame);

        if (ret < 0)
            break;

        if (!got_frame)
            return 0;
    }

    return ret;
}

static void parse_params(int argc, char **argv)
{
    g_options.mux = false;
    g_options.start_time = -1;
    g_options.end_time = -1;
    g_options.avisynth = false;
    g_options.frame_rate.num = 0;
    g_options.frame_rate.den = 0;

    for(int i=1; i<argc; i++)
    {
        if(argv[i][0] != '-')
        {
            char inputFile[kMaxPath];
            strcpy(inputFile, argv[i]);
            g_options.input_file = inputFile;
        }

        if(0 == strcmp(argv[i], "-m"))
            g_options.mux = true;

        if(0 == strcmp(argv[i], "-s"))
        {
            i++;
            g_options.start_time = atoi(argv[i]);
        }

        if(0 == strcmp(argv[i], "-e"))
        {
            i++;
            g_options.end_time = atoi(argv[i]);
        }

        if(0 == strcmp(argv[i], "-avisynth"))
        {
            ++i;
            g_options.avisynth = true;
            strcpy(g_options.avisynth_script, argv[i]);
        }

        if(0 == strcmp(argv[i], "-fr"))
        {
            char fr[16];
            ++i;
            strcpy(fr, argv[i]);
    
            char *p = strchr(fr, '/');

            if(p)
            {
                char num[10];
                char den[10];

                strncpy(num, fr, p-fr);
                strncpy(den, p+1, strlen(fr) - (p-fr));

                g_options.frame_rate.num = atoi(num);
                g_options.frame_rate.den = atoi(den);
            }
            else
            {
                g_options.frame_rate.num = atoi(fr);
                g_options.frame_rate.den = 1;
            }
        }
    }
}

/////////
// MAIN
/////////

int main(int argc, char **argv)
{
    int ret;

    if (argc < 2)
    {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s [-m] [-avisynth script] [-fps num/den] [-s time_in_sec] [-e time_in_sec] <input file>\n", argv[0]);
        return 1;
    }

    parse_params(argc, argv);

    av_register_all();

    avfilter_register_all();

    if ((ret = open_input_file(g_options.input_file)) < 0)
        goto end;

    if ((ret = open_output_files()) < 0)
        goto end;

#if USE_FILTER_GRAPH
    if ((ret = init_filters()) < 0)
        goto end;
#endif

    // Create a resampler for any input audio stream
    for(unsigned int i=0; i<g_ifmt_ctx->nb_streams; i++)
    {
        if(AVMEDIA_TYPE_AUDIO == g_ifmt_ctx->streams[i]->codec->codec_type &&
           g_stream_ctx[i].dec_ctx)
        {
            if (init_audio_resampler(g_stream_ctx[i].dec_ctx, g_stream_ctx[i].enc_ctx, &g_resampler_context))
                goto end;

            if (init_audio_fifo(&g_stream_ctx[i].audio_fifo, g_stream_ctx[i].enc_ctx))
                goto end;
        }
    }

    // Create the decode thread input queue, will contain demuxed packets
    ret = av_thread_message_queue_alloc(&g_decode_input_queue, THREAD_QUEUE_SIZE, sizeof(AVPacket));
    if (ret < 0)
        return ret;

    // Create the decode thread
    pthread_t decode_thread;

    if ((ret = pthread_create(&decode_thread, NULL, decode_thread_proc, NULL)))
    {
        av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));

        av_thread_message_queue_free(&g_decode_input_queue);

        return AVERROR(ret);
    }

    //int j = 0; // USED FOR TESTING

    // Demux: read all packets, put packets on decode_input_queue
    while (g_continue_audio_mutex.get() ||
           g_continue_video_mutex.get())
    {
        AVPacket packet;

        // Read a single packet from the file
        ret = av_read_frame(g_ifmt_ctx, &packet);

        if (ret == AVERROR(EAGAIN))
        {
            av_usleep(10000);
            continue;
        }

        if (ret < 0)
        {
            assert(ret == AVERROR_EOF);
            av_thread_message_queue_set_err_recv(g_decode_input_queue, ret);
            break;
        }

        /*  
        // USED FOR TESTING
        if(AVMEDIA_TYPE_VIDEO != ifmt_ctx->streams[packet.stream_index]->codec->codec_type)
        {
            av_packet_unref(&packet);
            continue;
        }
        */
        /*
        // USED FOR TESTING
        if(j++ == 20)
        {
            av_thread_message_queue_set_err_recv(decode_input_queue, AVERROR_EOF);
            av_packet_unref(&packet);
            break;
        }
        */

        // Put packet on decode_input_queue
        ret = av_thread_message_queue_send(g_decode_input_queue, &packet, g_flags);

        if (g_flags && ret == AVERROR(EAGAIN))
        {
            ret = av_thread_message_queue_send(g_decode_input_queue, &packet, 0);
            av_log(g_ifmt_ctx, AV_LOG_WARNING,
                   "decode_input_queue message queue blocking; consider raising the "
                   "thread_queue_size option (current value: %d)\n",
                   THREAD_QUEUE_SIZE);
        }

        if (ret < 0)
        {
            if (ret != AVERROR_EOF)
                av_log(g_ifmt_ctx, AV_LOG_ERROR,
                       "Unable to send packet to decode_input_queue: %s\n",
                       "error");//av_err2str(ret));
            av_packet_unref(&packet);
            av_thread_message_queue_set_err_recv(g_decode_input_queue, ret);
            break;
        }
    }

    // HERE FOR REFERENCE
    // Use the following kind of cleanup only for a drastic error where we dont
    //  exit by sending an EOS or error to the downstream queue.
    //AVPacket packet = {0};
    //av_thread_message_queue_set_err_send(decode_input_queue, AVERROR_EOF);
    //while (av_thread_message_queue_recv(decode_input_queue, &packet, 0) >= 0)
    //    av_packet_unref(&packet);

    // Wait for decode thread to exit
    pthread_join(decode_thread, NULL);

    av_thread_message_queue_free(&g_decode_input_queue);

    /* flush filters and encoders */
    for (unsigned int i = 0; i < g_ifmt_ctx->nb_streams; i++)
    {
        if(g_ifmt_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO &&
           g_ifmt_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;

        /* flush filter */
        if (g_filter_ctx && g_filter_ctx[i].filter_graph)
            ret = filter_convert_encode_write_frame(NULL, i);
        else
            ret = convert_encode_write_frame(NULL, i, NULL);

        /* flush encoders */
        ret = flush_encoder(i);

        if(ret == AVERROR_EOF)
            ret = 0;
    }

end:

    if(g_ifmt_ctx)
    {
        for (unsigned int i = 0; i < g_ifmt_ctx->nb_streams; i++)
        {
            if(g_stream_ctx[i].audio_fifo)
                av_audio_fifo_free(g_stream_ctx[i].audio_fifo);

            if(g_stream_ctx[i].dec_ctx)
                avcodec_free_context(&g_stream_ctx[i].dec_ctx);

            //if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            if(g_stream_ctx[i].enc_ctx)
                avcodec_free_context(&g_stream_ctx[i].enc_ctx);

            if (g_filter_ctx && g_filter_ctx[i].filter_graph)
                avfilter_graph_free(&g_filter_ctx[i].filter_graph);
        }
    }

    if(g_resampler_context)
        swr_free(&g_resampler_context);

    if(g_filter_ctx)
        av_free(g_filter_ctx);

    if(g_stream_ctx)
        av_free(g_stream_ctx);

    if(g_ifmt_ctx)
        avformat_close_input(&g_ifmt_ctx);

    for (int i=0; i<g_output_formats.size(); i++)
    {
        av_write_trailer(g_output_formats[i]);
        avio_closep(&g_output_formats[i]->pb);
        avformat_free_context(g_output_formats[i]);
    }

    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_make_error_string(g_error, AV_ERROR_MAX_STRING_SIZE, ret));

    return ret ? 1 : 0;
}
