
#ifdef ENABLE_FFMPEG_LOGGING
extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/common.h>
    #include <libavutil/time.h>
}
#endif

#include <stdio.h>  
#include <stdarg.h>  
#include <stdlib.h>

#include "utils.h"

#ifdef LINUX

#include <sys/time.h>

unsigned int timeGetTime()
{
    struct timeval now;
    gettimeofday(&now, 0);
    return now.tv_usec/1000;
}

#endif

#define FormatLogMessage(msg, fmt) { va_list args; va_start(args, fmt); vsnprintf(msg, sizeof(msg), fmt, args); strncat(msg, "\n", sizeof(msg)); va_end(args); }

static int gErrorCode = 0;

bool IsErrorSet()
{
    return (gErrorCode != 0);
}

void ClearError()
{
    gErrorCode = 0;
}

int GetErrorCode()
{
    return (gErrorCode);
}

void LogError(int errCode, const char *fmt, ...)
{
    char msg[kMaxSysStringLength];
    FormatLogMessage(msg, fmt);

    // TODO: Add event based logging
#ifdef ENABLE_FFMPEG_LOGGING
    av_log(NULL, AV_LOG_ERROR, msg);
#else
    printf(msg);
#endif

    gErrorCode = errCode;
}

void LogInfo(const char *fmt, ...)
{
    char msg[kMaxSysStringLength];
    FormatLogMessage(msg, fmt);

    // TODO: Add event based logging
#ifdef ENABLE_FFMPEG_LOGGING
    av_log(NULL, AV_LOG_INFO, msg);
#else
    printf(msg);
#endif
}

void LogWarning(const char *fmt, ...)
{
    char msg[kMaxSysStringLength];
    FormatLogMessage(msg, fmt);

    // TODO: Add event based logging
#ifdef ENABLE_FFMPEG_LOGGING
    av_log(NULL, AV_LOG_WARNING, msg);
#else
    printf(msg);
#endif
}

void LogStatus(const char *fmt, ...)
{
    char msg[kMaxSysStringLength];
    FormatLogMessage(msg, fmt);

    // TODO: Add event based logging
#ifdef ENABLE_FFMPEG_LOGGING
    av_log(NULL, AV_LOG_INFO, msg);
#else
    printf(msg);
#endif
}

void LogProgress(const char *msg, int progress)
{
    char progressMsg[kMaxSysStringLength];

    snprintf(progressMsg, sizeof(progressMsg), "%s: %d percent complete\n", msg, progress);

    // TODO: Add event based logging
#ifdef ENABLE_FFMPEG_LOGGING
    av_log(NULL, AV_LOG_INFO, progressMsg);
#else
    printf(progressMsg);
#endif
}

ContinueMutex::ContinueMutex()
{
    pthread_mutex_init(&m_mutex, NULL);
    m_continue = true;
}

ContinueMutex::~ContinueMutex()
{
    pthread_mutex_destroy(&m_mutex);
    m_continue = false;
}

bool ContinueMutex::get()
{
    bool b_ret = true;

    pthread_mutex_lock(&m_mutex);
    b_ret = m_continue;
    pthread_mutex_unlock(&m_mutex);

    return b_ret;
}

void ContinueMutex::set(bool b_continue)
{
    pthread_mutex_lock(&m_mutex);
    m_continue = b_continue;
    pthread_mutex_unlock(&m_mutex);
}
