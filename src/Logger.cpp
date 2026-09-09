// The loader log. Opened in DllMain before any graphics work happens, and
// closed when the DLL unloads.
//
// Everything about this is chosen so that it cannot itself become a problem: a
// plain FILE*, one critical section, no allocation, no dependency on anything
// else in the project, and a silent no-op if the file could not be opened. A
// logger that can fail is worse than no logger, because it fails at exactly the
// moment something else has gone wrong.

#include "Logger.h"
#include <ctime>

// Kept in step with the build rather than typed here, so a bug report's first
// line always names the build it came from.
#ifndef MWON12_VERSION_STRING
#  define MWON12_VERSION_STRING "0.0.0-dev"
#endif

// Everything written here is plain ASCII on purpose. This file carries no BOM
// and is read as ANSI by Notepad and by most people pasting it into a bug
// report, so a UTF-8 em-dash arrives as three bytes of mojibake.

FILE*            Logger::s_file = nullptr;
CRITICAL_SECTION Logger::s_cs  = {};
bool             Logger::s_init = false;

void Logger::Init(const char* path)
{
    InitializeCriticalSection(&s_cs);
    fopen_s(&s_file, path, "w");
    s_init = true;
    Log("MWOn12 v%s (DirectX 9 -> DirectX 12)  -  log opened.",
        MWON12_VERSION_STRING);
}

void Logger::Log(const char* fmt, ...)
{
    // Silently do nothing if Init never ran or the file could not be opened.
    // A read-only game directory is a real possibility and is not a reason to
    // stop the game from starting.
    if (!s_init || !s_file) return;

    // Timestamp read outside the lock; it only describes this line.
    SYSTEMTIME st{};
    GetLocalTime(&st);

    // The game is multi-threaded, so the format-and-write sequence has to be
    // atomic or lines from different threads interleave mid-sentence.
    EnterCriticalSection(&s_cs);
    fprintf(s_file, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_file, fmt, ap);
    va_end(ap);

    fputc('\n', s_file);
    // Flush every line rather than letting stdio buffer. If the game crashes,
    // the last few lines are the ones worth having, and they are exactly the
    // ones a buffer would swallow.
    fflush(s_file);
    LeaveCriticalSection(&s_cs);
}

void Logger::Shutdown()
{
    if (s_file) { fclose(s_file); s_file = nullptr; }
    if (s_init) { DeleteCriticalSection(&s_cs); s_init = false; }
}
