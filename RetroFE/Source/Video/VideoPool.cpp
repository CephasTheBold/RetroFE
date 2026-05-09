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
uint32_t VideoPool::currentGeneration_ = 0;

namespace {
    constexpr size_t POOL_BUFFER_INSTANCES = 2;
}

VideoPool::VideoPtr VideoPool::createNewVideo(int monitor, bool softOverlay) {
    auto vid = std::make_shared<GStreamerVideo>(monitor);
    if (!vid || vid->hasError()) return nullptr;

    vid->setGeneration(currentGeneration_); // Tag new videos
    vid->setSoftOverlay(softOverlay);
    return vid;
}

VideoPool::VideoPtr VideoPool::popBestAvailable(PoolInfo& pool, int monitor, int listId) {
    if (pool.available.empty()) return nullptr;

    // 1. Search from FRONT to BACK (Oldest to Newest).
    // The oldest videos have had the most frames to finish their cleanup.
    auto it = pool.available.begin();
    while (it != pool.available.end()) {
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(it->get())) {
            if (gsv->isReadyForReuse()) {
                // Found one that is already done!
                auto vid = std::move(*it);
                pool.available.erase(it);
                pool.currentActive++;
                return vid;
            }
        }
        ++it;
    }

    // 2. FALLBACK: If nothing is "instantly" ready, take the oldest one 
    // and perform the synchronous wait. This is still better than 
    // waiting on the one that was JUST released.
    auto vid = std::move(pool.available.front());
    pool.available.erase(pool.available.begin());

    if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
        gsv->prepareForReuse(); // Blocks only as a last resort
    }

    pool.currentActive++;
    return vid;
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (shuttingDown_) return nullptr;

    // Non-pooled (listId -1)
    if (listId == -1) {
        return createNewVideo(monitor, softOverlay);
    }

    PoolInfo& pool = pools_[monitor][listId];

    if (pool.markedForCleanup) {
        LOG_DEBUG("VideoPool", "Acquire (bail: cleanup) " + poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    // 1) Try to reuse an existing "warm" video
    if (auto vid = popBestAvailable(pool, monitor, listId)) {
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    // 2) Growth Logic: If we haven't hit our latched max, create a new one.
    if (!pool.initialCountLatched ||
        (pool.currentActive + pool.available.size() < pool.requiredInstanceCount)) {

        pool.currentActive++;

        // Track the max concurrency during the "pre-latch" phase
        if (!pool.initialCountLatched) {
            pool.observedMaxActive = std::max(pool.observedMaxActive, pool.currentActive);
        }

        auto vid = createNewVideo(monitor, softOverlay);
        if (!vid) {
            if (pool.currentActive > 0) pool.currentActive--;
            LOG_WARNING("VideoPool", "Acquire (New create FAIL) " + poolStateStr(monitor, listId, pool));
            return nullptr;
        }

        LOG_DEBUG("VideoPool", "Acquire (New instance created) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 3) At Capacity: If the pool is "full" but something is idle, steal it.
    if (!pool.available.empty()) {
        auto vid = std::move(pool.available.back());
        pool.available.pop_back();
        pool.currentActive++;

        vid->setSoftOverlay(softOverlay);
        LOG_DEBUG("VideoPool", "Acquire (MaxCap steal) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 4) Emergency Fallback: If we are fully drained, create one anyway to avoid a black screen.
    pool.currentActive++;
    return createNewVideo(monitor, softOverlay);
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid) return;
    if (shuttingDown_) return;

    // One-shot videos are simply dropped; shared_ptr will destroy them 
    // once background callbacks finish.
    if (listId == -1) return;

    if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
        // ALWAYS trigger a stop/unload command.
        // This tells background threads to finish up and release their refs.
        gsv->unload();

        if (gsv->getGeneration() != currentGeneration_) {
            LOG_DEBUG("VideoPool", "Discarding old generation video.");
            return; // The shared_ptr goes out of scope and memory is freed soon.
        }
    }

    PoolInfo& pool = pools_[monitor][listId];

    // If the list was closed/cleaned up, drop the reference.
    if (pool.markedForCleanup) {
        if (pool.currentActive > 0) pool.currentActive--;
        if (pool.currentActive == 0) erasePoolIfIdle_nolock(monitor, listId);
        return;
    }

    if (pool.currentActive > 0) {
        pool.currentActive--;
    }

    // --- THE EVICTION CEILING ---
    if (pool.initialCountLatched) {
        // If our idle cache is already at the required capacity, destroy this instance.
        if (pool.available.size() >= pool.requiredInstanceCount) {
            LOG_DEBUG("VideoPool", "Evicting excess video. Pool capacity reached.");
            // We return early; 'vid' shared_ptr goes out of scope and destroys the object.
            return;
        }
    }

    // Cache the video for reuse.
    pool.available.push_back(std::move(vid));

    // Latch logic: Calculate the steady-state max once we stop seeing new concurrent requests.
    if (!pool.initialCountLatched) {
        pool.requiredInstanceCount = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
        pool.initialCountLatched = true;
        LOG_DEBUG("VideoPool", "Release (LATCHED cap=" + std::to_string(pool.requiredInstanceCount) + ") " +
            poolStateStr(monitor, listId, pool));
    }
    else {
        LOG_DEBUG("VideoPool", "Release (Cache for reuse) " + poolStateStr(monitor, listId, pool));
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

    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    lit->second.markedForCleanup = true;
    erasePoolIfIdle_nolock(monitor, listId);

    LOG_DEBUG("VideoPool", "Marked for cleanup: Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
}

void VideoPool::shutdown() {
    LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
    shuttingDown_ = true;

    // Dropping the map releases the Pool's shared_ptr references.
    // Background threads still holding a reference via CallbackCtx will finish 
    // their work before the objects are physically deleted.
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
        (p.initialCountLatched ? " LATCHED" : " PRELATCH");
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (shuttingDown_) return;
    PoolInfo& pool = pools_[monitor][listId];
    pool.requiredInstanceCount = std::max(pool.requiredInstanceCount, desiredTotal);
}

void VideoPool::reset() {
    LOG_INFO("VideoPool", "Reconstructing pools (Generation " + std::to_string(currentGeneration_ + 1) + ")");
    currentGeneration_++;
    pools_.clear(); // Immediately destroys all cached (available) videos
}