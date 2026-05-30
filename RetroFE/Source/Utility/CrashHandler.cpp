#include "CrashHandler.h"
#include "Utils.h"           
#include "SDL.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring> // For strlen

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
// Instruct the compiler to link dbghelp.lib dynamically on Windows
#pragma comment(lib, "dbghelp.lib") 
#else
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>     // For low-level open()
#include <execinfo.h>  // For backtrace and backtrace_symbols_fd
#include <csignal>
#endif

namespace fs = std::filesystem;

// Static tracking pointer to bridge native OS callbacks to our handler context safely
static CrashHandler* g_CrashHandlerInstance = nullptr;

// A fallback string to keep path generation safe if global state hasn't initialized
static std::string g_EmergencyLogPath = "log.txt";

void CrashHandler::initialize() {
    // Allocation of a persistent static instance on the first initialization call
    static CrashHandler instance;
    g_CrashHandlerInstance = &instance;

#ifdef _WIN32
    SetUnhandledExceptionFilter(reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(windowsExceptionFilter));
#else
    std::signal(SIGSEGV, linuxSignalHandler);
    std::signal(SIGFPE, linuxSignalHandler);
    std::signal(SIGILL, linuxSignalHandler);
    std::signal(SIGABRT, linuxSignalHandler);
    std::signal(SIGBUS, linuxSignalHandler);
#endif

    // Pre-calculate emergency log path while the system state is healthy
    try {
        fs::path baseDir;
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(NULL, buffer, MAX_PATH);
        baseDir = fs::path(buffer).parent_path();
        g_EmergencyLogPath = Utils::combinePath(baseDir.parent_path().string(), "log.txt");
#else
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            baseDir = fs::path(buffer).parent_path();
            g_EmergencyLogPath = Utils::combinePath(baseDir.string(), "log.txt");
        }
#endif
    }
    catch (...) {
        g_EmergencyLogPath = "log.txt";
    }
}

void CrashHandler::logAndShowCrash(const std::string& errorReason, const std::string& stackTrace) {
    std::stringstream fullAlert;
    fullAlert << "RetroFE has encountered a fatal crash.\n\n"
        << "Reason: " << errorReason << "\n\n"
        << "Call Stack Context:\n" << stackTrace;

    // Force unbuffered stream writing straight to disk
    std::ofstream logFile(g_EmergencyLogPath, std::ios::app);
    if (logFile.is_open()) {
        logFile << "\n==================================================\n";
        logFile << "[CRITICAL] [FATAL CRASH DETECTED]\n";
        logFile << fullAlert.str();
        logFile << "==================================================\n";
        logFile.flush(); // Commit data cache blocks straight to the storage kernel
        logFile.close();
    }

    // CABINET SAFETY: Kill audio subsystem instantly. 
    // Prevents screeching, looping, or stuttering sound streams from blasting through the cabinet speakers.
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_SetRelativeMouseMode(SDL_FALSE); // Restore desktop cursor control if locked down

    // Display standard fallback error window modal
    std::string popupMsg = "RetroFE has encountered a fatal error (" + errorReason + ").\n\n"
        "A diagnostic call stack trace has been forced to your log file.\n"
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
    if (!g_CrashHandlerInstance) return 1; // EXCEPTION_EXECUTE_HANDLER

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

    g_CrashHandlerInstance->logAndShowCrash(reason, ss.str());
    return 1; // EXCEPTION_EXECUTE_HANDLER
}
#else
void CrashHandler::linuxSignalHandler(int signalNumber) {
    // ASYNC-SIGNAL-SAFE ONLY ZONE. No stringstreams, no std::string, no malloc/free.

    // 1. Instantly drop low-level file descriptor to disk bypassing std::ofstream buffering locks
    int fd = open(g_EmergencyLogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        const char* header = "\n==================================================\n"
            "[CRITICAL] [FATAL UNIX SIGNAL DETECTED]\n"
            "Reason: ";
        write(fd, header, strlen(header));

        switch (signalNumber) {
            case SIGSEGV: write(fd, "SIGSEGV (Segmentation Fault)\n", 29); break;
            case SIGFPE:  write(fd, "SIGFPE (Arithmetic Error)\n", 26); break;
            case SIGILL:  write(fd, "SIGILL (Illegal Instruction)\n", 29); break;
            case SIGABRT: write(fd, "SIGABRT (Abort Script Call)\n", 28); break;
            case SIGBUS:  write(fd, "SIGBUS (Bus Alignment Error)\n", 29); break;
            default:      write(fd, "Unknown System Signal\n", 22); break;
        }

        // 2. Capture stack addresses into our trace pool buffer
        void* stackBuffer[64];
        int frames = backtrace(stackBuffer, 64);

        const char* traceHeader = "Stack Trace Call Frames:\n";
        write(fd, traceHeader, strlen(traceHeader));

        // 3. Write directly to disk descriptor. Bypasses memory allocation completely!
        backtrace_symbols_fd(stackBuffer, frames, fd);

        const char* footer = "==================================================\n";
        write(fd, footer, strlen(footer));

#ifdef __linux__
        fsync(fd); // Push raw disk blocks to hardware instantly on Linux
#endif
        close(fd);
    }

    // 4. Kill audio loops so they don't lock sound streams up on the machine speakers
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    // 5. Hard immediate kernel bypass drop. Safe from deadlock states.
    _exit(EXIT_FAILURE);
}
#endif

#ifdef _WIN32
void createMinidump(struct _EXCEPTION_POINTERS* exceptionInfo, const std::string& baseDirectory) {
    // 1. Construct a unique filename based on the current system time
    SYSTEMTIME localTime;
    GetLocalTime(&localTime);

    std::string dumpPath = Utils::combinePath(baseDirectory, "retrofe_crash.dmp");

    // 2. Create the file on disk using native Windows API
    HANDLE hFile = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        // 3. Bind the exception pointers so the dump knows exactly which thread faulted
        MINIDUMP_EXCEPTION_INFORMATION dumpExceptionInfo;
        dumpExceptionInfo.ThreadId = GetCurrentThreadId();
        dumpExceptionInfo.ExceptionPointers = exceptionInfo;
        dumpExceptionInfo.ClientPointers = TRUE;

        // 4. Write the dump file
        // MiniDumpWithDataSegs gives you a great balance: captures global state variables 
        // without ballooning the file size like a full memory dump would.
        BOOL success = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            MiniDumpWithDataSegs,
            &dumpExceptionInfo,
            nullptr,
            nullptr
        );

        CloseHandle(hFile);
    }
}
#endif