#pragma once

#include <pthread.h>

#ifdef WINDOWS
    #include <windows.h>
#endif

#ifdef LINUX
    unsigned int timeGetTime();
#endif

#define kMissingFileError -100
#define kRunProcessError -101
#define kMaxSysStringLength 4096
#define kMaxPath MAX_PATH

bool IsErrorSet();
void ClearError();
int GetErrorCode();
void LogError(int errCode, const char *fmt, ...);

void LogInfo(const char *fmt, ...);
void LogWarning(const char *fmt, ...);

void LogStatus(const char *fmt, ...);
void LogProgress(const char *msg, int progress);

int RunProcess(const char *executable, const char *args);

class ContinueMutex
{
    public:

    ContinueMutex();
    ~ContinueMutex();
    bool get();
    void set(bool b_continue);

    private:

    pthread_mutex_t m_mutex;
    bool m_continue;
};
