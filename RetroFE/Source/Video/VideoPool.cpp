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

#include "VideoPool.h"
#include "../Utility/Log.h"
#include "GStreamerVideo.h"
#include "IVideo.h"

#include <algorithm>
#include <chrono>

VideoPool::PoolMap VideoPool::pools_{};
bool VideoPool::shuttingDown_ = false;

namespace {
    constexpr size_t POOL_BUFFER_INSTANCES = 2;
}

VideoPool::VideoPtr VideoPool::createNewVideo(int monitor, bool softOverlay) {
    auto vid = std::make_unique<GStreamerVideo>(monitor);
    if (!vid || vid->hasError()) return nullptr;
    vid->setSoftOverlay(softOverlay);
    return vid;
}

VideoPool::VideoPtr VideoPool::popBestAvailable(PoolInfo& pool, int monitor, int listId) {
    if (pool.available.empty()) {
        return nullptr;
    }

    // LIFO: Grab the absolute most recently released video from the very back.
    // O(1) instant removal, keeps CPU caches hot, lets OS page out the cold ones.
    auto vid = std::move(pool.available.back());
    pool.available.pop_back();

    pool.currentActive++;
    LOG_DEBUG("VideoPool", "Acquire (LIFO reuse) " + poolStateStr(monitor, listId, pool));

    return vid;
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (shuttingDown_) return nullptr;

    // Non-pooled
    if (listId == -1) {
        return createNewVideo(monitor, softOverlay);
    }

    PoolInfo& pool = pools_[monitor][listId];

    if (pool.markedForCleanup) {
        // If caller tries to acquire while pool is being cleaned up, treat as "no video".
        // (Alternative policy: ignore cleanup flag and keep serving until fully idle.)
        LOG_DEBUG("VideoPool", "Acquire (bail: cleanup) " + poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    // 1) Reuse if possible
    if (auto vid = popBestAvailable(pool, monitor, listId)) {
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    // 2) Pre-latch behavior: always create so we can observe peak concurrency.
    if (!pool.initialCountLatched) {
        pool.currentActive++;
        pool.observedMaxActive = std::max(pool.observedMaxActive, pool.currentActive);

        auto vid = createNewVideo(monitor, softOverlay);
        if (!vid) {
            if (pool.currentActive > 0) pool.currentActive--;
            LOG_WARNING("VideoPool", "Acquire (PreLatch create FAIL) " + poolStateStr(monitor, listId, pool));
            return nullptr;
        }

        LOG_DEBUG("VideoPool", "Acquire (PreLatch create OK) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 3) Grow if under required capacity
    const size_t total = pool.currentActive + pool.available.size();
    if (total < pool.requiredInstanceCount) {
        pool.currentActive++;

        auto vid = createNewVideo(monitor, softOverlay);
        if (!vid) {
            if (pool.currentActive > 0) pool.currentActive--;
            LOG_WARNING("VideoPool", "Acquire (Growth create FAIL) " + poolStateStr(monitor, listId, pool));
            return nullptr;
        }

        LOG_DEBUG("VideoPool", "Acquire (Growth create OK) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 4) At cap: if anything is available at all, steal most-recent (even if “cold”)
    // (This means we never block; we always return something if there is anything available.)
    if (!pool.available.empty()) {
        auto it = std::prev(pool.available.end());
        auto vid = std::move(*it);
        pool.available.erase(it);
        pool.currentActive++;

        LOG_DEBUG("VideoPool", "Acquire (MaxCap take-most-recent) " + poolStateStr(monitor, listId, pool));
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    // 5) Fully drained and at cap: policy choice.
    // For UI smoothness, prefer creating (rare, but avoids returning nullptr).
    pool.currentActive++;
    auto vid = createNewVideo(monitor, softOverlay);
    if (!vid) {
        if (pool.currentActive > 0) pool.currentActive--;
        LOG_WARNING("VideoPool", "Acquire (AtCap create FAIL) " + poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    LOG_DEBUG("VideoPool", "Acquire (AtCap create OK) " + poolStateStr(monitor, listId, pool));
    return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid) return;
    if (shuttingDown_) return;

    // Non-pooled videos: just drop them
    if (listId == -1) return;

    PoolInfo& pool = pools_[monitor][listId];

    // Safety: if caller releases to a pool that is being cleaned up, just drop.
    if (pool.markedForCleanup) {
        if (pool.currentActive > 0) pool.currentActive--;

        if (pool.currentActive == 0) {
            erasePoolIfIdle_nolock(monitor, listId);
        }
        return;
    }

    // Unload BEFORE returning to the pool
    if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
        gsv->unload();
    }

    if (pool.currentActive > 0) pool.currentActive--;
    else {
        LOG_WARNING("VideoPool", "Release (currentActive already 0) " + poolStateStr(monitor, listId, pool));
    }

    // --- THE EVICTION CEILING ---
    // If we have finished the warmup phase (latched), we enforce a strict memory cap.
    if (pool.initialCountLatched) {
        // requiredInstanceCount already includes the POOL_BUFFER_INSTANCES.
        // If our idle cache is already full, destroy this video.
        if (pool.available.size() >= pool.requiredInstanceCount) {
            LOG_DEBUG("VideoPool", "Evicting excess video. Pool is at capacity (" +
                std::to_string(pool.requiredInstanceCount) + ").");
            // Returning early causes 'vid' (a unique_ptr) to fall out of scope and be destroyed!
            return;
        }
    }

    // If we haven't hit the ceiling, cache it for reuse.
    pool.available.push_back(std::move(vid));

    // Latch logic (calculates the ceiling)
    if (!pool.initialCountLatched) {
        const size_t candidate = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
        pool.requiredInstanceCount = std::max(pool.requiredInstanceCount, candidate);
        pool.initialCountLatched = true;

        LOG_DEBUG("VideoPool", "Release (latch required=" + std::to_string(pool.requiredInstanceCount) + ") " +
            poolStateStr(monitor, listId, pool));
    }
    else {
        LOG_DEBUG("VideoPool", "Release (reuse) " + poolStateStr(monitor, listId, pool));
    }

    // If we were marked for cleanup and became idle, erase now
    if (pool.markedForCleanup && pool.currentActive == 0) {
        erasePoolIfIdle_nolock(monitor, listId);
    }
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
    if (shuttingDown_ || listId == -1) return;
    for (auto& vid : videos) {
        releaseVideo(std::move(vid), monitor, listId);
    }
}

void VideoPool::cleanup(int monitor, int listId) {
    if (shuttingDown_) return;
    if (listId == -1) return;

    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    PoolInfo& pool = lit->second;
    pool.markedForCleanup = true;

    erasePoolIfIdle_nolock(monitor, listId);

    LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) +
        ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
    LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
    shuttingDown_ = true;

    // Drop all pooled instances (unique_ptr destructors run on main thread)
    pools_.clear();

    LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

void VideoPool::erasePoolIfIdle_nolock(int monitor, int listId) {
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    auto& pool = lit->second;
    if (!pool.markedForCleanup || pool.currentActive != 0) return;

    pool.available.clear();

    mit->second.erase(lit);
    if (mit->second.empty()) {
        pools_.erase(mit);
    }
}

std::string VideoPool::poolStateStr(int monitor, int listId, const PoolInfo& p) {
    return "Mon:" + std::to_string(monitor) +
        " List:" + std::to_string(listId) +
        " Active=" + std::to_string(p.currentActive) +
        " Avail=" + std::to_string(p.available.size()) +
        " Req=" + std::to_string(p.requiredInstanceCount) +
        (p.initialCountLatched ? " LATCHED" : " PRELATCH") +
        (p.markedForCleanup ? " CLEANUP" : "");
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (shuttingDown_) return;
    if (listId == -1) return;

    PoolInfo& pool = pools_[monitor][listId];
    pool.requiredInstanceCount = std::max(pool.requiredInstanceCount, desiredTotal);
}