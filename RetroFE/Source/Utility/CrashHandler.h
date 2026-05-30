#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <string>

class CrashHandler {
public:
    static void initialize();

private:
    // Flushes forensic details to log.txt immediately and triggers an SDL popup
    static void logAndShowCrash(const std::string& errorReason, const std::string& stackTrace);

#ifdef _WIN32
    // Windows Structured Exception Handling Callback
    static long __stdcall windowsExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo);
#else
    // Linux POSIX Signal Callback
    static void linuxSignalHandler(int signalNumber);
#endif
};

#endif // CRASH_HANDLER_H