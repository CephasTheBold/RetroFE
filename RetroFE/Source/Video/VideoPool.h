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

#pragma once

#include <list>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

class IVideo;

// Main-thread-only pool for IVideo instances.
// listId == -1 => non-pooled ("one-shot") videos.
class VideoPool {
public:
    using VideoPtr = std::unique_ptr<IVideo>;

    static VideoPtr acquireVideo(int monitor, int listId, bool softOverlay);
    static void releaseVideo(VideoPtr vid, int monitor, int listId);
    static void releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId);

    // Marks a pool for cleanup. If there are no active instances, the pool is erased immediately.
    static void cleanup(int monitor, int listId);

    // Clears all pools and disables further pooling.
    static void shutdown();

    // Hint the pool that we'd like at least desiredTotal instances available+active.
    // This does not pre-create; it just raises the max to avoid later "at cap" behavior.
    static void reserveCapacity(int monitor, int listId, size_t desiredTotal);

    static bool isShuttingDown() { return shuttingDown_; }
    static bool shuttingDown_;


private:
    struct PoolInfo {
        // Oldest-first; we push releases to the back.
        std::list<VideoPtr> available;

        size_t currentActive = 0;
        size_t observedMaxActive = 0;

        // Once latched, this becomes the “steady state max total” (active + available).
        size_t requiredInstanceCount = 0;
        bool initialCountLatched = false;

        bool markedForCleanup = false;
    };

    using ListPoolMap = std::unordered_map<int, PoolInfo>;
    using PoolMap = std::unordered_map<int, ListPoolMap>;

    static PoolMap pools_;

    static void erasePoolIfIdle_nolock(int monitor, int listId);
    static std::string poolStateStr(int monitor, int listId, const PoolInfo& p);

    // Policy: pick the best available instance:
    // 1) most recently released "warm" (actualState != None)
    // 2) oldest "cold" (actualState == None)
    static VideoPtr popBestAvailable(PoolInfo& pool, int monitor, int listId);

    // Policy: create a new instance; returns nullptr on failure.
    static VideoPtr createNewVideo(int monitor, bool softOverlay);
};