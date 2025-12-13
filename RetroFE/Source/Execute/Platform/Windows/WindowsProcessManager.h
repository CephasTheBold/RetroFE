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
#pragma once

#include <Windows.h>   // For HANDLE, DWORD, HWND, etc.
#include <string>
#include <set>
#include <functional>

#include "../IProcessManager.h"

 /**
  * @brief A Windows-specific implementation of the IProcessManager interface.
  *
  * This class handles the launching, monitoring, and termination of processes
  * using the Windows API, including Job Objects for robust child-process cleanup
  * and a fallback mechanism to find fullscreen game windows.
  */
class WindowsProcessManager : public IProcessManager {
public:
    WindowsProcessManager();
    ~WindowsProcessManager() override;

    bool simpleLaunch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    bool launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    WaitResult wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) override;
    void terminate() override;
    bool tryGetExitCode(int& outExitCode) const override;

    WindowsProcessManager(const WindowsProcessManager&) = delete;
    WindowsProcessManager& operator=(const WindowsProcessManager&) = delete;

private:
    // --- Private Helper Methods ---
    static std::string getExeNameFromHwnd(HWND hwnd);
    static void logFullscreenCheckDetails(HWND hwnd);

    void terminateProcessTree(DWORD processId, std::set<DWORD>& processedIds);

    bool requestGracefulShutdownForPid(DWORD pid, DWORD waitMsTotal);

    // NOTE: Signature unchanged; implementation now treats success as "primary PID exited",
    // instead of "all job members exited".
    bool requestGracefulShutdownForJob(DWORD waitMsTotal);

    static bool isWindowFullscreen(HWND hwnd);
    static bool isSteamHelperWindow(HWND hwnd);

    void cleanupHandles();
    bool isRunning() const;

private:
    HWND        hRetroFEWindow_ = nullptr;
    HANDLE      hProcess_ = nullptr;
    HANDLE      hJob_ = nullptr;
    bool        jobAssigned_ = false;
    std::string executableName_;
    std::string workingDirectory_;
    DWORD       processId_ = 0;
};

#endif
