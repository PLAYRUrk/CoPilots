#pragma once
// Log.h — thread-safe logging to X-Plane's Log.txt via XPLMDebugString
// All messages are prefixed with [CoPilots].

#include <cstdarg>

namespace cp {

// Printf-style logging. Safe to call from any thread (internally serialised).
void Log(const char* fmt, ...);
void LogWarning(const char* fmt, ...);
void LogError(const char* fmt, ...);

} // namespace cp
