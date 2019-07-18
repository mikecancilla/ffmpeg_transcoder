#pragma once

#include <vector>

extern "C"
{
    #include <libavfilter/avfilter.h>
    #include <libavcodec/avcodec.h>
}

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;

int init_filters(void);