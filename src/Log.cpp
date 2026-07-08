#include "Log.h"
#include <XPLM/XPLMUtilities.h>
#include <cstdio>
#include <mutex>
#include <string>

namespace cp {

static std::mutex s_logMutex;

static void write(const char* level, const char* fmt, va_list args)
{
    char buf[2048];
    char msg[1920];
    vsnprintf(msg, sizeof(msg), fmt, args);
    snprintf(buf, sizeof(buf), "[CoPilots]%s %s\n", level, msg);

    std::lock_guard<std::mutex> lock(s_logMutex);
    XPLMDebugString(buf);
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write("", fmt, args);
    va_end(args);
}

void LogWarning(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write("[WARN]", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write("[ERROR]", fmt, args);
    va_end(args);
}

}
