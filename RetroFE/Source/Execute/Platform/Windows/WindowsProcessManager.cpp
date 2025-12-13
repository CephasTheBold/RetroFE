/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef WIN32

#include "WindowsProcessManager.h"

#include <fstream>
#include <filesystem>
#include <vector>
#include <chrono>
#include <cstdlib>   // getenv
#include <Psapi.h>   // GetModuleFileNameExA
#include <Tlhelp32.h>
#include <SDL2/SDL_syswm.h>

#include "../../../SDL.h"                 // SDL::getWindow
#include "../../../Utility/Log.h"
#include "../../../Utility/Utils.h"       // Utils::toLower
#include "../../../Database/Configuration.h"

namespace {
    std::string getExeNameFromPath(const std::string& path) {
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) {
            return path.substr(pos + 1);
        }
        return path;
    }

    static bool isMameExeName(const std::string& exeName) {
        std::string n = Utils::toLower(exeName);
        return (n.rfind("mame", 0) == 0); // starts with "mame"
    }
}

// Collect all top-level windows that belong to a PID
static void collectWindowsForPid(DWORD pid, std::vector<HWND>& out) {
    struct Ctx { DWORD pid; std::vector<HWND>* out; };
    auto thunk = [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<Ctx*>(lParam);
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid == ctx->pid && IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            ctx->out->push_back(hwnd);
        }
        return TRUE;
        };
    Ctx ctx{ pid, &out };
    EnumWindows(thunk, reinterpret_cast<LPARAM>(&ctx));
}

// Politely ask each window to close (fast/non-blocking first; small bounded fallback)
static void sendCloseToWindows(const std::vector<HWND>& windows) {
    for (HWND h : windows) {
        // Fast path: async requests (doesn't burn your grace budget)
        PostMessage(h, WM_SYSCOMMAND, SC_CLOSE, 0);
        PostMessage(h, WM_CLOSE, 0, 0);

        // Small bounded fallback (optional but helps stubborn windows)
        SendMessageTimeout(h, WM_CLOSE, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, nullptr);
    }
}

static bool waitForHandleExitBounded(HANDLE h, DWORD waitMsTotal) {
    if (!h) return false;

    const DWORD slice = 100;
    DWORD waited = 0;

    while (waited < waitMsTotal) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            return true;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
        waited += slice;
    }
    return false;
}

// Best-effort, bounded wait for a single PID to exit
static bool waitForPidExitBounded(DWORD pid, DWORD waitMsTotal) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;

    const DWORD slice = 100;
    DWORD waited = 0;

    while (waited < waitMsTotal) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            return true;
        }

        // keep UI responsive
        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
        waited += slice;
    }

    CloseHandle(h);
    return false;
}

// Graceful ask for a single PID
bool WindowsProcessManager::requestGracefulShutdownForPid(DWORD pid, DWORD waitMsTotal) {
    std::vector<HWND> hwnds;
    collectWindowsForPid(pid, hwnds);
    if (!hwnds.empty()) {
        LOG_INFO("ProcessManager", "Sending close to " + std::to_string(hwnds.size()) +
            " window(s) for PID " + std::to_string(pid));
        sendCloseToWindows(hwnds);
        return waitForPidExitBounded(pid, waitMsTotal);
    }

    // No windows — can't send WM_CLOSE; treat as not gracefully closable here.
    return false;
}

// Graceful ask for all processes in our Job.
// IMPORTANT: success condition is now: "primary PID exited within the budget"
// rather than "every job member exited".
bool WindowsProcessManager::requestGracefulShutdownForJob(DWORD waitMsTotal) {
    if (!hJob_) return false;

    // --- Query required buffer size for the process ID list ---
    JOBOBJECT_BASIC_PROCESS_ID_LIST header{ 0 };
    DWORD bytes = 0;
    (void)QueryInformationJobObject(
        hJob_,
        JobObjectBasicProcessIdList,
        &header,
        sizeof(header),
        &bytes);

    if (bytes == 0) {
        return false;
    }

    std::vector<BYTE> buf(bytes);
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
    if (!QueryInformationJobObject(hJob_, JobObjectBasicProcessIdList, list, bytes, &bytes)) {
        return false;
    }

    LOG_INFO("ProcessManager",
        "Job members: " + std::to_string(list->NumberOfProcessIdsInList) +
        " (primary=" + std::to_string(processId_) + ")");

    // --- Ask all job members (that have windows) to close ---
    for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        DWORD pid = static_cast<DWORD>(list->ProcessIdList[i]);

        std::vector<HWND> hwnds;
        collectWindowsForPid(pid, hwnds);

        if (!hwnds.empty()) {
            LOG_INFO("ProcessManager",
                "Requesting close for job member PID " + std::to_string(pid) +
                " (" + std::to_string(hwnds.size()) + " window(s))");
            sendCloseToWindows(hwnds);
        }
        else {
            LOG_DEBUG("ProcessManager",
                "Job member PID " + std::to_string(pid) + " has no visible top-level windows; skipping WM_CLOSE.");
        }
    }

    // --- Success condition: primary process exits within the budget ---
    if (processId_ == 0) {
        LOG_WARNING("ProcessManager", "requestGracefulShutdownForJob: primary PID is 0; cannot wait.");
        return false;
    }

    // Prefer waiting on our owned process handle (more reliable than OpenProcess-by-PID).
    if (hProcess_) {
        const DWORD slice = 100;
        DWORD waited = 0;

        while (waited < waitMsTotal) {
            if (WaitForSingleObject(hProcess_, 0) == WAIT_OBJECT_0) {
                return true;
            }

            // Keep UI responsive and consistent with the rest of your wait loops.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                DispatchMessage(&msg);
            }

            waited += slice;
        }

        return false;
    }

    // Fallback: if for some reason we don't have hProcess_, do PID-based wait.
    return waitForPidExitBounded(processId_, waitMsTotal);
}


WindowsProcessManager::WindowsProcessManager() {
    LOG_INFO("ProcessManager", "WindowsProcessManager created.");

    SDL_SysWMinfo winfo;
    SDL_VERSION(&winfo.version);

    SDL_Window* mainWindow = SDL::getWindow(0);
    if (mainWindow != nullptr && SDL_GetWindowWMInfo(mainWindow, &winfo) == SDL_TRUE) {
        hRetroFEWindow_ = winfo.info.win.window;
    }
    else {
        hRetroFEWindow_ = nullptr;
    }
}

WindowsProcessManager::~WindowsProcessManager() {
    cleanupHandles();
}

void WindowsProcessManager::cleanupHandles() {
    if (hProcess_ != nullptr) {
        CloseHandle(hProcess_);
        hProcess_ = nullptr;
    }
    if (hJob_ != nullptr) {
        CloseHandle(hJob_);
        hJob_ = nullptr;
    }
    jobAssigned_ = false;
    processId_ = 0;
    executableName_.clear();
    workingDirectory_.clear();
}

bool WindowsProcessManager::simpleLaunch(const std::string& executable,
    const std::string& args,
    const std::string& currentDirectory) {
    std::string ext = Utils::toLower(std::filesystem::path(executable).extension().string());
    bool isBatch = (ext == ".bat" || ext == ".cmd");

    std::string commandLine;
    std::string workDir = currentDirectory;

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (isBatch) {
        char* comspec = std::getenv("COMSPEC");
        std::string shell = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
        commandLine = "\"" + shell + "\" /C \"\"" + executable + "\"";
        if (!args.empty()) commandLine += " " + args;
        commandLine += "\"";
    }
    else {
        commandLine = "\"" + executable + "\"";
        if (!args.empty()) commandLine += " " + args;
    }

    if (!CreateProcessA(
        nullptr,
        &commandLine[0],
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si,
        &pi))
    {
        LOG_ERROR("ProcessManager", "simpleLaunch failed: " + commandLine +
            " (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool WindowsProcessManager::launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) {
    cleanupHandles();

    std::filesystem::path exePath(executable);
    if (!exePath.is_absolute()) exePath = std::filesystem::absolute(exePath);

    std::filesystem::path currDir(currentDirectory);
    if (!currDir.is_absolute()) currDir = std::filesystem::absolute(currDir);

    std::string exePathStr = exePath.string();
    std::string currDirStr = currDir.string();
    executableName_ = getExeNameFromPath(exePathStr);
    workingDirectory_ = currDirStr;

    LOG_INFO("ProcessManager", "Attempting to launch: " + exePathStr);
    if (!args.empty()) LOG_INFO("ProcessManager", "           Arguments: " + args);
    LOG_INFO("ProcessManager", "     Working directory: " + currDirStr);

    // Create/config job
    hJob_ = CreateJobObject(NULL, NULL);
    if (hJob_ == NULL) {
        LOG_ERROR("ProcessManager", "Failed to create Job Object. Error: " + std::to_string(GetLastError()));
    }
    else {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            LOG_WARNING("ProcessManager", "Failed to set Job Object limits. Error: " + std::to_string(GetLastError()));
        }
    }

    std::string extension = Utils::toLower(exePath.extension().string());
    bool isExe = (extension == ".exe");
    bool isBat = (extension == ".bat" || extension == ".cmd");
    bool launchCommandSent = false;

    if (isExe || isBat) {
        STARTUPINFOA startupInfo{};
        PROCESS_INFORMATION processInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.wShowWindow = SW_SHOWDEFAULT;

        std::string commandLine;

        if (isExe) {
            commandLine = "\"" + exePathStr + "\"";
            if (!args.empty()) commandLine += " " + args;
        }
        else {
            const char* comspec = std::getenv("COMSPEC");
            std::string shell = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
            commandLine = "\"" + shell + "\" /C \"\"" + exePathStr + "\"";
            if (!args.empty()) commandLine += " " + args;
            commandLine += "\"";
        }

        DWORD flags = CREATE_NO_WINDOW;
        if (CreateProcessA(nullptr, &commandLine[0], nullptr, nullptr,
            TRUE, flags, nullptr, currDirStr.c_str(),
            &startupInfo, &processInfo))
        {
            launchCommandSent = true;
            hProcess_ = processInfo.hProcess;
            processId_ = processInfo.dwProcessId;

            if (hJob_ && AssignProcessToJobObject(hJob_, hProcess_)) {
                jobAssigned_ = true;
                LOG_INFO("ProcessManager", "Process assigned to Job Object.");
            }
            else if (hJob_) {
                LOG_ERROR("ProcessManager", "Failed to assign process to Job Object. Error: " + std::to_string(GetLastError()));
            }

            CloseHandle(processInfo.hThread);
        }
        else {
            LOG_ERROR("ProcessManager", "CreateProcess failed for: " + commandLine +
                " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }
    else {
        SHELLEXECUTEINFOA shExInfo = { 0 };
        shExInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        shExInfo.lpVerb = "open";
        shExInfo.lpFile = exePathStr.c_str();
        shExInfo.lpParameters = args.c_str();
        shExInfo.lpDirectory = currDirStr.c_str();
        shExInfo.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExA(&shExInfo)) {
            launchCommandSent = true;
            if (shExInfo.hProcess) {
                hProcess_ = shExInfo.hProcess;
                processId_ = GetProcessId(hProcess_);

                if (hJob_ != NULL && AssignProcessToJobObject(hJob_, hProcess_)) {
                    jobAssigned_ = true;
                    LOG_INFO("ProcessManager", "Process (from ShellExecuteEx) assigned to Job Object.");
                }
                else if (hJob_ != NULL) {
                    LOG_WARNING("ProcessManager", "Failed to assign process from ShellExecuteEx to Job Object.");
                }
            }
            else {
                LOG_INFO("ProcessManager", "ShellExecute did not return a process handle. Detection will occur in the wait phase.");
            }
        }
        else {
            LOG_ERROR("ProcessManager", "ShellExecuteEx failed for: " + exePathStr +
                " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }

    return launchCommandSent;
}

WaitResult WindowsProcessManager::wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) {
    bool isDetecting = !isRunning();
    if (isDetecting) {
        LOG_INFO("ProcessManager", "Entering detection phase (UI will remain active)...");
    }
    else {
        LOG_INFO("ProcessManager", "Process handle already acquired. Entering monitoring phase...");
    }

    auto monitoringStartTime = std::chrono::steady_clock::now();
    auto lastDetectionTime = monitoringStartTime;
    HWND lastLoggedHwnd = nullptr;

    while (true) {
        if (onFrameTick) onFrameTick();
        if (userInputCheck && userInputCheck()) return WaitResult::UserInput;

        if (isDetecting) {
            auto now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDetectionTime).count() > 250) {
                const int focusGracePeriodSec = 5;
                auto elapsedGrace = std::chrono::duration_cast<std::chrono::seconds>(now - monitoringStartTime).count();
                HWND foregroundHwnd = GetForegroundWindow();

                if (elapsedGrace > focusGracePeriodSec && foregroundHwnd == hRetroFEWindow_) {
                    LOG_WARNING("ProcessManager", "Focus returned to RetroFE after grace period; assuming launch failed.");
                    return WaitResult::Error;
                }

                if (foregroundHwnd) {
                    DWORD pid;
                    GetWindowThreadProcessId(foregroundHwnd, &pid);

                    if (pid != GetCurrentProcessId() && IsWindowVisible(foregroundHwnd)) {
                        if (isSteamHelperWindow(foregroundHwnd)) {
                            if (foregroundHwnd != lastLoggedHwnd) {
                                LOG_DEBUG("ProcessManager", "Ignoring known launcher window (Steam).");
                                lastLoggedHwnd = foregroundHwnd;
                            }
                        }
                        else {
                            if (isWindowFullscreen(foregroundHwnd)) {
                                HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
                                if (hProc) {
                                    char windowTitle[256] = { 0 };
                                    GetWindowTextA(foregroundHwnd, windowTitle, sizeof(windowTitle));
                                    std::string exeName = getExeNameFromHwnd(foregroundHwnd);

                                    LOG_INFO("ProcessManager", "Detection successful. Found fullscreen game process (PID: " +
                                        std::to_string(pid) + ", Title: \"" + std::string(windowTitle) + "\", EXE: " + exeName + ").");

                                    LOG_INFO("ProcessManager", "Forcing detected window to the foreground.");
                                    SetForegroundWindow(foregroundHwnd);

                                    hProcess_ = hProc;
                                    processId_ = pid;
                                    executableName_ = exeName;
                                    jobAssigned_ = false;

                                    LOG_INFO("ProcessManager", "Transitioning to monitoring phase.");
                                    isDetecting = false;
                                }
                            }
                            else {
                                if (foregroundHwnd != lastLoggedHwnd) {
                                    logFullscreenCheckDetails(foregroundHwnd);
                                    lastLoggedHwnd = foregroundHwnd;
                                }
                            }
                        }
                    }
                }

                lastDetectionTime = now;
            }
        }
        else {
            if (WaitForSingleObject(hProcess_, 0) == WAIT_OBJECT_0) {
                return WaitResult::ProcessExit;
            }
        }

        if (timeoutSeconds > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - monitoringStartTime).count() >= timeoutSeconds)
            {
                return WaitResult::Timeout;
            }
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
    }
}

void WindowsProcessManager::terminate() {
    // General grace vs MAME grace
    constexpr DWORD kGraceWaitMsGeneral = 3000;
    constexpr DWORD kGraceWaitMsMame = 8000;

    if (!isRunning()) {
        LOG_WARNING("ProcessManager", "Terminate called but no process was running.");
        cleanupHandles();
        return;
    }

    const bool isMame = isMameExeName(executableName_);
    const DWORD graceBudget = isMame ? kGraceWaitMsMame : kGraceWaitMsGeneral;

    bool createdMameTrigger = false;
    std::filesystem::path mameTriggerPath;

    auto removeTriggerIfNeeded = [&]() {
        if (!createdMameTrigger) return;
        std::error_code ec;
        std::filesystem::remove(mameTriggerPath, ec);
        };

    const auto t0 = std::chrono::steady_clock::now();
    auto remainingMs = [&]() -> DWORD {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= static_cast<long long>(graceBudget)) return 0;
        return static_cast<DWORD>(graceBudget - elapsed);
        };

    // --- MAME special-case: ask it to quit via trigger file first ---
    if (isMame && !workingDirectory_.empty()) {
        mameTriggerPath = std::filesystem::path(workingDirectory_) / "mame_exit.trigger";

        std::ofstream out(mameTriggerPath.string(), std::ios::out | std::ios::trunc);
        if (out.good()) {
            out.close();
            createdMameTrigger = true;
            LOG_INFO("ProcessManager", "MAME detected - wrote exit trigger: " + mameTriggerPath.string());

            DWORD rem = remainingMs();
            if (rem > 0 && waitForPidExitBounded(processId_, rem)) {
                LOG_INFO("ProcessManager", "MAME exited cleanly after trigger.");
                removeTriggerIfNeeded();
                cleanupHandles();
                return;
            }

            // Remove trigger so next launch doesn't instantly quit.
            {
                std::error_code ec;
                std::filesystem::remove(mameTriggerPath, ec);
                if (ec) {
                    LOG_WARNING("ProcessManager", "Failed to remove MAME exit trigger (" + mameTriggerPath.string() + "): " + ec.message());
                }
                else {
                    LOG_INFO("ProcessManager", "MAME did not exit in time; removed exit trigger and escalating.");
                }
            }
        }
        else {
            LOG_WARNING("ProcessManager", "MAME detected but could not write exit trigger: " + mameTriggerPath.string());
        }
    }

    // --- If we have a job, prefer job-based graceful shutdown ---
    if (jobAssigned_ && hJob_) {
        LOG_INFO("ProcessManager", "Attempting graceful shutdown for job (primary PID " + std::to_string(processId_) + ")...");

        DWORD rem = remainingMs();
        if (rem > 0 && requestGracefulShutdownForJob(rem)) {
            LOG_INFO("ProcessManager", "Graceful job shutdown succeeded (primary exited).");
            removeTriggerIfNeeded();
            cleanupHandles();
            return;
        }

        // Edge timing: if it exited while we were attempting, treat as success.
        if (!isRunning()) {
            LOG_INFO("ProcessManager", "Primary process already exited; treating as graceful success.");
            removeTriggerIfNeeded();
            cleanupHandles();
            return;
        }

        LOG_WARNING("ProcessManager", "Graceful job shutdown failed; escalating to TerminateJobObject.");
        removeTriggerIfNeeded();
        TerminateJobObject(hJob_, 1);
        cleanupHandles();
        return;
    }

    // --- No job: graceful shutdown for the single PID ---
    LOG_INFO("ProcessManager", "Attempting graceful shutdown for PID " + std::to_string(processId_) + "...");
    {
        DWORD rem = remainingMs();
        if (rem > 0 && requestGracefulShutdownForPid(processId_, rem)) {
            LOG_INFO("ProcessManager", "Graceful shutdown succeeded.");
            removeTriggerIfNeeded();
            cleanupHandles();
            return;
        }
    }

    // Edge timing re-check
    if (!isRunning()) {
        LOG_INFO("ProcessManager", "Primary process already exited; treating as graceful success.");
        removeTriggerIfNeeded();
        cleanupHandles();
        return;
    }

    LOG_WARNING("ProcessManager", "Graceful shutdown failed; terminating process tree.");
    removeTriggerIfNeeded();

    std::set<DWORD> processedIds;
    terminateProcessTree(processId_, processedIds);
    cleanupHandles();
}

bool WindowsProcessManager::tryGetExitCode(int& outExitCode) const {
    if (!hProcess_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(hProcess_, &code)) return false;
    if (code == STILL_ACTIVE) return false;
    outExitCode = static_cast<int>(code);
    return true;
}

bool WindowsProcessManager::isRunning() const {
    if (hProcess_ == nullptr) return false;
    DWORD exitCode = 0;
    return (GetExitCodeProcess(hProcess_, &exitCode) && exitCode == STILL_ACTIVE);
}

std::string WindowsProcessManager::getExeNameFromHwnd(HWND hwnd) {
    if (!hwnd) return "";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return "";

    char exePath[MAX_PATH] = { 0 };
    std::string exeName;
    if (GetModuleFileNameExA(hProc, NULL, exePath, MAX_PATH)) {
        exeName = getExeNameFromPath(exePath);
    }
    CloseHandle(hProc);
    return exeName;
}

void WindowsProcessManager::logFullscreenCheckDetails(HWND hwnd) {
    if (!hwnd) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::string exeName = getExeNameFromHwnd(hwnd);

    RECT appBounds;
    if (!GetWindowRect(hwnd, &appBounds)) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: GetWindowRect failed for " + exeName);
        return;
    }

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: MonitorFromWindow failed for " + exeName);
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: GetMonitorInfo failed for " + exeName);
        return;
    }

    char windowTitle[256] = { 0 };
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));

    std::string windowRectStr = "L:" + std::to_string(appBounds.left) +
        " T:" + std::to_string(appBounds.top) +
        " R:" + std::to_string(appBounds.right) +
        " B:" + std::to_string(appBounds.bottom);

    std::string monitorRectStr = "L:" + std::to_string(monitorInfo.rcMonitor.left) +
        " T:" + std::to_string(monitorInfo.rcMonitor.top) +
        " R:" + std::to_string(monitorInfo.rcMonitor.right) +
        " B:" + std::to_string(monitorInfo.rcMonitor.bottom);

    LOG_DEBUG("ProcessManager", "Fullscreen Check Failed for \"" + std::string(windowTitle) +
        "\" (PID: " + std::to_string(pid) + ", EXE: " + exeName +
        "). Window: {" + windowRectStr + "} | Monitor: {" + monitorRectStr + "}");
}

void WindowsProcessManager::terminateProcessTree(DWORD processId, std::set<DWORD>& processedIds) {
    if (processedIds.count(processId)) return;
    processedIds.insert(processId);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32{};
        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe32)) {
            do {
                if (pe32.th32ParentProcessID == processId) {
                    terminateProcessTree(pe32.th32ProcessID, processedIds);
                }
            } while (Process32Next(hSnap, &pe32));
        }
        CloseHandle(hSnap);
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (hProc) {
        LOG_DEBUG("ProcessManager", "Terminating PID: " + std::to_string(processId));
        TerminateProcess(hProc, 1);
        CloseHandle(hProc);
    }
}

bool WindowsProcessManager::isWindowFullscreen(HWND hwnd) {
    if (!hwnd) return false;

    RECT appBounds;
    if (!GetWindowRect(hwnd, &appBounds)) return false;

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) return false;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) return false;

    const int tolerance = 4;

    long windowWidth = appBounds.right - appBounds.left;
    long windowHeight = appBounds.bottom - appBounds.top;
    long monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    long monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

    if (std::abs(windowWidth - monitorWidth) <= tolerance &&
        std::abs(windowHeight - monitorHeight) <= tolerance)
    {
        if (std::abs(appBounds.left - monitorInfo.rcMonitor.left) <= tolerance &&
            std::abs(appBounds.top - monitorInfo.rcMonitor.top) <= tolerance)
        {
            return true;
        }
    }

    // Overscan/negative-margin fullscreen
    if (appBounds.left <= monitorInfo.rcMonitor.left &&
        appBounds.top <= monitorInfo.rcMonitor.top &&
        appBounds.right >= monitorInfo.rcMonitor.right &&
        appBounds.bottom >= monitorInfo.rcMonitor.bottom)
    {
        return true;
    }

    return false;
}

bool WindowsProcessManager::isSteamHelperWindow(HWND hwnd) {
    if (!hwnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return false;

    char exePath[MAX_PATH] = { 0 };
    bool result = false;
    if (GetModuleFileNameExA(hProc, NULL, exePath, MAX_PATH)) {
        std::string exeName = getExeNameFromPath(exePath);
        if (_stricmp(exeName.c_str(), "steamwebhelper.exe") == 0 ||
            _stricmp(exeName.c_str(), "steam.exe") == 0)
        {
            result = true;
        }
    }

    CloseHandle(hProc);
    return result;
}

#endif
