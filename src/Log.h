#pragma once

#include <cstdarg>

namespace cp {

void Log(const char* fmt, ...);
void LogWarning(const char* fmt, ...);
void LogError(const char* fmt, ...);

}
