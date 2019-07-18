#pragma once

extern "C"
{
    #include <libavutil/frame.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
}

bool WriteFrame(AVCodecContext *dec_ctx, AVFrame *frame, int frame_num);