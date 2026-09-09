// The loader log — MWOn12.log, written beside the game executable.
//
// This is deliberately the smallest possible logger, and it is separate from
// the renderer's own log for a reason. It has to work before any graphics
// device exists, it must never fail, and it holds exactly one thing: which
// backend was requested, which one is actually running, and why those differ.
// That is the first question to ask about any report of "MWOn12 isn't working",
// and keeping it in a short file of its own means the answer is not buried
// under thousands of lines of render tracing.
//
// The detailed diagnostic stream lives in MWOn12-render.log and is produced by
// the renderer's logger instead.
//
// Every line is flushed as it is written. A crash is exactly the case where
// the log matters most, and buffered output is lost precisely then.

#pragma once
#include <cstdio>
#include <cstdarg>
#include <windows.h>

class Logger {
public:
    static void Init(const char* path);
    static void Log(const char* fmt, ...);
    static void Shutdown();

private:
    static FILE*             s_file;
    static CRITICAL_SECTION  s_cs;
    static bool              s_init;
};
