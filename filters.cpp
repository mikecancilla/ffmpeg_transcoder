#include <stdlib.h>
#include "ffmpeg_transcoder.h"
#include "filters.h"
#include "fr_conversion.h"

extern "C"
{
    #include <libavutil/opt.h>
    #include <libavformat/avformat.h>
}

extern AVFormatContext *g_ifmt_ctx;
extern StreamContext *g_stream_ctx;
extern FilteringContext *g_filter_ctx;
extern Options g_options;

static int init_filter(FilteringContext *fctx,
                       AVStream *st,
                       AVCodecContext *enc_ctx,
                       const char *filter_spec)
{
    char args[512];
    int ret = 0;

    const AVFilter *cur_filter = NULL;
    AVFilterContext *cur_ctx = NULL;
    AVFilterContext *prev_ctx = NULL;

//    AVCodecContext *codec_ctx;
//    ret = avcodec_parameters_to_context(codec_ctx, st->codecpar);

    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
    {
        av_log(NULL, AV_LOG_ERROR, "Can not create filter graph\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    fctx->filter_graph = filter_graph;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        // Create Video Source
        //////////////////////
        cur_filter = avfilter_get_by_name("buffer");

        if (!cur_filter)
        {
            av_log(NULL, AV_LOG_ERROR, "Filter video source not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
            st->codecpar->width, st->codecpar->height,
            st->codecpar->format,
            st->time_base.num, st->time_base.den,
            st->codecpar->sample_aspect_ratio.num, st->codecpar->sample_aspect_ratio.den,
            st->r_frame_rate.num, st->r_frame_rate.den);

        ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "src",
                                           args, NULL, filter_graph);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create video source\n");
            goto end;
        }

        fctx->buffersrc_ctx = cur_ctx;
        prev_ctx = cur_ctx;

        // enc_ctx num and den are flipped at this point, store into dst un-flipped
        AVRational dst;
        dst.num = enc_ctx->time_base.den;
        dst.den = enc_ctx->time_base.num;

        // TODO: Pull this in from the json.
        bool bIsTelecine = false;
        EFrameRateConversionCode fr_code = CalculateFrameRateConversion(st->r_frame_rate, dst, bIsTelecine);

        // Perform deinterlacing?
        /////////////////////////
        if( IsDeinterlacing(fr_code) )
        {
            cur_filter = avfilter_get_by_name("yadif");

            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter yadif not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "yadif",
                                               NULL, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create yadif filter\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link yadif filter\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Perform frame rate conversion?
        /////////////////////////////////
        if(st->r_frame_rate.num != dst.num ||
           st->r_frame_rate.den != dst.den)
        {
            /*
            st->codec->field_order

            enum AVFieldOrder {
                AV_FIELD_UNKNOWN,
                AV_FIELD_PROGRESSIVE,
                AV_FIELD_TT,          //< Top coded_first, top displayed first
                AV_FIELD_BB,          //< Bottom coded first, bottom displayed first
                AV_FIELD_TB,          //< Top coded first, bottom displayed first
                AV_FIELD_BT,          //< Bottom coded first, top displayed first
            };
            */

            // All of the following code is based on CSourceAssembly::ConfigureAVISynthFRConverter()

            /*
            if (m_bSourceIsVFR)
            {
                // TODO: For the time being, don't convert VFR between PAL and NTSC.
                // Currently the VFR->CFR is done in the MC FR Converter.
                // Converting between NTSC/PAL here would lose sync.
                m_hr = E_INVALIDARG;
                SetError(ERR_UnsupportedFrameRate, L"BuildVideoPreprocessingPath", L"VFR unsupported in PAL<->NTSC conversion");
                goto ConfigureAVISynthFRConverter_Failed;
            }
            */

            if(fr_code == kNTSCInverseTelecine_to_PAL ||
               fr_code == kNTSCInverseTelecine_to_NTSCFilm ||
               fr_code == kNTSC60pInverseTelecine_to_NTSCFilm ||
               fr_code == kNTSC60pInverseTelecine_to_PAL)
            {
                //double frameRate = 23.976;
                if (fr_code == EFrameRateConversionCode::kNTSC60pInverseTelecine_to_NTSCFilm ||
                    fr_code == EFrameRateConversionCode::kNTSC60pInverseTelecine_to_PAL)
                {
                    // TODO
                    //wcscpy_s(script, _countof(script), L"TDecimate(cycleR=3, Cycle=5)");

                    // Insert Decimate
                    /*
                    cur_filter = avfilter_get_by_name("decimate");

                    if (!cur_filter)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Filter decimate not found\n");
                        ret = AVERROR_UNKNOWN;
                        goto end;
                    }

                    ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "decimate",
                                                       NULL, NULL, filter_graph);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot create decimate filter\n");
                        goto end;
                    }

                    ret = avfilter_link(prev_ctx, 0, cur_ctx, 0);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot link decimate filter\n");
                        goto end;
                    }

                    prev_ctx = cur_ctx;
                    */
                }
                else
                {
                    //wcscpy_s(script, _countof(script), L"TFM()TDecimate()");

                    // Insert FieldMatch
                    cur_filter = avfilter_get_by_name("fieldmatch");

                    if (!cur_filter)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Filter fieldmatch not found\n");
                        ret = AVERROR_UNKNOWN;
                        goto end;
                    }

                    ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "fieldmatch",
                                                       NULL, NULL, filter_graph);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot create fieldmatch filter\n");
                        goto end;
                    }

                    ret = avfilter_link(prev_ctx, 0, cur_ctx, 0);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot link fieldmatch filter\n");
                        goto end;
                    }

                    prev_ctx = cur_ctx;

                    // Insert Decimate
                    cur_filter = avfilter_get_by_name("decimate");

                    if (!cur_filter)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Filter decimate not found\n");
                        ret = AVERROR_UNKNOWN;
                        goto end;
                    }

                    ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "decimate",
                                                       NULL, NULL, filter_graph);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot create decimate filter\n");
                        goto end;
                    }

                    ret = avfilter_link(prev_ctx, 0, cur_ctx, 0);

                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Cannot link decimate filter\n");
                        goto end;
                    }

                    prev_ctx = cur_ctx;
                }

                // Speed up video to 25fps if desired (e.g. NTSC film to PAL conversion)
                if (fr_code == EFrameRateConversionCode::kNTSCInverseTelecine_to_PAL ||
                    fr_code == EFrameRateConversionCode::kNTSC60pInverseTelecine_to_PAL)
                {
                    //wcscat_s(script, _countof(script), L"AssumeFPS(25, 1, sync_audio=false)");
                    //frameRate = 25.0;
                    dst.num = 25;
                    dst.den = 1;
                }
                else
                {
                    dst.num = 24000;
                    dst.den = 1001;
                }

                // Update output frame rate
                // TODO: code review -- move otu to BuildFilterGraph everything that changes the output frame 
                //if (m_pTUM->GetFrameRatePassThru() && m_iTranscodeStage == TranscodeStages::kMainContentStage)
                //    m_pTUM->SetOutputFrameRate(frameRate);
            }
            else if (fr_code == kNTSC60p_to_PAL)
            {
                /*
                // Perform frame rate conversion to 50i
                wcscpy_s(script, _countof(script), L"ChangeFPS(50.00)\n"); // or blend via ConvertFPS

                                                                           // Re-interlace
                bool isTFF = m_pTUM->IsInterlacedTopFieldFirst();
                wcscat_s(script, _countof(script), (isTFF) ? L"AssumeTFF()\n" : L"AssumeBFF()\n");
                wcscat_s(script, _countof(script), L"SeparateFields()\n");
                wcscat_s(script, _countof(script), L"SelectEvery(4,0,3)\n");
                wcscat_s(script, _countof(script), L"Weave()\n");

                wcscat_s(script, _countof(script), L"AssumeFPS(25, 1, sync_audio=false)");
                */
            }

            cur_filter = avfilter_get_by_name("fps");
            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter fps not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            char fps[32];
            char num[10];
            char den[10];
            snprintf(num, sizeof(num), "%d", dst.num);
            snprintf(den, sizeof(den), "%d", dst.den);
            strcpy(fps, num);
            strcat(fps, "/");
            strcat(fps, den);

            snprintf(args, sizeof(args), "fps=%s", fps);

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "fps",
                                               args, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create fps\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link fps\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Create avisynth?
        ///////////////////
        if (g_options.avisynth)
        {
            cur_filter = avfilter_get_by_name("avisynth");
            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter avisynth not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            snprintf(args, sizeof(args),
                     "script=%s",
                     g_options.avisynth_script);

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "avisynth",
                                               args, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create avisynth\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link avisynth\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Create Video Scaler?
        ///////////////////////
        if (enc_ctx->width != st->codecpar->width || enc_ctx->height != st->codecpar->height)
        {
            cur_filter = avfilter_get_by_name("scale");
            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter video_scale not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            snprintf(args, sizeof(args),
                     "width=%d:height=%d",
                     enc_ctx->width, enc_ctx->height);

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "scale",
                                               args, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create video scaler\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link video scaler\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Create Video Trim?
        /////////////////////
        if (g_options.start_time != -1 || g_options.end_time != -1)
        {
            cur_filter = avfilter_get_by_name("trim");
            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter video_trim not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            memset(args, 0, sizeof(args));

            if(g_options.start_time != -1)
            {
                snprintf(args, sizeof(args),
                         "start=%d",
                         g_options.start_time);
            }

            if(g_options.end_time != -1)
            {
                char end[32];
                snprintf(end, sizeof(end),
                         (g_options.start_time != -1) ? ":end=%d" : "end=%d",
                         g_options.end_time);
                strcat(args, end);
            }

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "video_trim",
                                               args, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create video trim\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link video trim\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Create Video Sink
        ////////////////////
        cur_filter = avfilter_get_by_name("buffersink");
        if (!cur_filter)
        {
            av_log(NULL, AV_LOG_ERROR, "Filter video sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "sink",
                                           NULL, NULL, filter_graph);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create video sink\n");
            goto end;
        }

        ret = av_opt_set_bin(cur_ctx, "pix_fmts",
                             (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                             AV_OPT_SEARCH_CHILDREN);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }

        ret = avfilter_link(prev_ctx, 0,
                            cur_ctx, 0);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot link video sink\n");
            goto end;
        }

        fctx->buffersink_ctx = cur_ctx;
    }
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        // Create the audio source
        //////////////////////////
        cur_filter = avfilter_get_by_name("abuffer");
        if (!cur_filter)
        {
            av_log(NULL, AV_LOG_ERROR, "Filter audio source not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%llx",
                 st->time_base.num, st->time_base.den, st->codecpar->sample_rate,
                 av_get_sample_fmt_name((AVSampleFormat) st->codecpar->format),
                 st->codecpar->channel_layout);

        ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "src",
                                           args, NULL, filter_graph);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio source\n");
            goto end;
        }

        fctx->buffersrc_ctx = cur_ctx;
        prev_ctx = cur_ctx;

        // Create audio trim?
        /////////////////////
        if (g_options.start_time != -1 || g_options.end_time != -1)
        {
            cur_filter = avfilter_get_by_name("atrim");
            if (!cur_filter)
            {
                av_log(NULL, AV_LOG_ERROR, "Filter audio_trim not found\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            memset(args, 0, sizeof(args));

            if(g_options.start_time != -1)
            {
                snprintf(args, sizeof(args),
                         "start=%d",
                         g_options.start_time);
            }

            if(g_options.end_time != -1)
            {
                char end[32];
                snprintf(end, sizeof(end),
                         (g_options.start_time != -1) ? ":end=%d" : "end=%d",
                         g_options.end_time);
                strcat(args, end);
            }

            ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "audio_trim",
                                               args, NULL, filter_graph);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot create audio trim\n");
                goto end;
            }

            ret = avfilter_link(prev_ctx, 0,
                                cur_ctx, 0);

            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Cannot link audio trim\n");
                goto end;
            }

            prev_ctx = cur_ctx;
        }

        // Create the audio sink
        ////////////////////////
        cur_filter = avfilter_get_by_name("abuffersink");
        if (!cur_filter)
        {
            av_log(NULL, AV_LOG_ERROR, "Filter audio sink not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avfilter_graph_create_filter(&cur_ctx, cur_filter, "sink",
                                           NULL, NULL, filter_graph);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio sink\n");
            goto end;
        }

        ret = avfilter_link(prev_ctx, 0,
                            cur_ctx, 0);

        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot link audio sink\n");
            goto end;
        }

        fctx->buffersink_ctx = cur_ctx;
    }
    else
    {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    return ret;
}

int init_filters(void)
{
    const char *filter_spec;
    unsigned int i;
    int ret;

    g_filter_ctx = (FilteringContext *)av_malloc_array(g_ifmt_ctx->nb_streams, sizeof(*g_filter_ctx));

    if (!g_filter_ctx)
        return AVERROR(ENOMEM);

#ifdef DEBUG
    char p_graph[2048] = {0};
#endif

    for (i = 0; i < g_ifmt_ctx->nb_streams; i++)
    {
        g_filter_ctx[i].buffersrc_ctx = NULL;
        g_filter_ctx[i].buffersink_ctx = NULL;
        g_filter_ctx[i].filter_graph = NULL;

        if (!(g_ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
            || g_ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;

        if (g_ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null"; /* passthrough (dummy) filter for video */
        else
            filter_spec = "anull"; /* passthrough (dummy) filter for audio */

        ret = init_filter(&g_filter_ctx[i],
                          g_ifmt_ctx->streams[i],
                          g_stream_ctx[i].enc_ctx,
                          filter_spec);

        if (ret)
            return ret;

#ifdef DEBUG
        strcpy(p_graph, avfilter_graph_dump(g_filter_ctx[i].filter_graph, NULL));
#endif
    }

    return 0;
}
