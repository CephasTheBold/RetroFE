#include "CrashHandler.h"
#include "SDL.h"
#include <iostream>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#include <execinfo.h> 
#endif

void CrashHandler::initialize() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(windowsExceptionFilter));
#else
    std::signal(SIGSEGV, linuxSignalHandler);
    std::signal(SIGFPE, linuxSignalHandler);
    std::signal(SIGILL, linuxSignalHandler);
    std::signal(SIGABRT, linuxSignalHandler);
#endif
}

void CrashHandler::logAndShowCrash(const std::string& errorReason, const std::string& stackTrace) {
    std::stringstream fullAlert;
    fullAlert << "RetroFE has encountered a fatal crash.\n\n"
        << "Reason: " << errorReason << "\n\n"
        << "Call Stack Context:\n" << stackTrace;

    // 1. Force an unbuffered file write to disk immediately (bypasses memory logging lag)
    std::ofstream logFile("log.txt", std::ios::app);
    if (logFile.is_open()) {
        logFile << "\n==================================================\n";
        logFile << "[CRITICAL] [FATAL CRASH DETECTED]\n";
        logFile << fullAlert.str();
        logFile << "==================================================\n";
        logFile.close();
    }

    // 2. Cross-platform popup container
    std::string popupMsg = "RetroFE has encountered a fatal error (" + errorReason + ").\n\n"
        "A diagnostic call stack trace has been forced to log.txt.\n"
        "Please send your layout configurations and log.txt to the developer.";

    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "RetroFE - Fatal Application Error",
        popupMsg.c_str(),
        nullptr
    );
}

#ifdef _WIN32
long __stdcall CrashHandler::windowsExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    std::string reason = "Unknown Hard Exception";

    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:   reason = "EXCEPTION_ACCESS_VIOLATION (Memory Fault / Null Pointer Dereference)"; break;
        case EXCEPTION_STACK_OVERFLOW:     reason = "EXCEPTION_STACK_OVERFLOW (Stack Exhaustion)"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: reason = "EXCEPTION_INT_DIVIDE_BY_ZERO (Math/Division Error)"; break;
    }

    std::stringstream ss;
    ss << "Fault address pointer: 0x" << std::hex << reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress) << "\n";

    void* stackBuffer[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stackBuffer, nullptr);
    ss << "Stack Trace (" << frames << " frames captured):\n";
    for (USHORT i = 0; i < frames; ++i) {
        ss << "  [" << i << "] Frame Offset: 0x" << std::hex << reinterpret_cast<uintptr_t>(stackBuffer[i]) << "\n";
    }

    logAndShowCrash(reason, ss.str());
    return 1; // EXCEPTION_EXECUTE_HANDLER
}
#else
void CrashHandler::linuxSignalHandler(int signalNumber) {
    std::string reason = "Unknown OS Signal";

    switch (signalNumber) {
        case SIGSEGV: reason = "SIGSEGV (Segmentation Fault / Invalid Memory Reference)"; break;
        case SIGFPE:  reason = "SIGFPE (Fatal Arithmetic Exception / Floating Point Failure)"; break;
        case SIGILL:  reason = "SIGILL (Illegal Instruction / Core Corruption)"; break;
        case SIGABRT: reason = "SIGABRT (Abort Broadcast, likely via std::terminate / Unhandled C++ Exception)"; break;
    }

    std::stringstream ss;
    void* stackBuffer[64];
    int frames = backtrace(stackBuffer, 64);
    char** symbols = backtrace_symbols(stackBuffer, frames);

    ss << "Stack Trace (" << frames << " frames captured):\n";
    if (symbols) {
        for (int i = 0; i < frames; ++i) {
            ss << "  [" << i << "] " << symbols[i] << "\n";
        }
        free(symbols);
    }
    else {
        for (int i = 0; i < frames; ++i) {
            ss << "  [" << i << "] Frame Offset: 0x" << std::hex << reinterpret_cast<uintptr_t>(stackBuffer[i]) << "\n";
        }
    }

    logAndShowCrash(reason, ss.str());
    _exit(EXIT_FAILURE);
}
#endif