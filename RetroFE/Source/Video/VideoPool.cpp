/* This file is part of RetroFE.
* ... [License Header] ...
*/

#include "VideoPool.h"
#include "../Utility/Log.h"
#include "../Utility/ThreadPool.h"
#include <algorithm>
#include <memory>
#include <deque>
#include <unordered_map>

std::mutex VideoPool::s_mutex;
std::condition_variable VideoPool::s_cv;
std::atomic<bool> VideoPool::shuttingDown_ = false;
VideoPool::PoolMap VideoPool::pools_;

namespace {
    constexpr size_t POOL_BUFFER_INSTANCES = 2;
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        auto vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) return nullptr; // non-pooled caller should handle
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    std::unique_lock<std::mutex> lk(s_mutex);
    PoolInfo& pool = pools_[monitor][listId];

    auto try_pop_reuse = [&]() -> VideoPtr {
        // 1. THE HOT SWAP: Explicitly hunt for active pipelines (triggers instant-uri!)
        // We iterate backwards from the end to grab the MOST RECENTLY released pipeline.
        if (!pool.available.empty()) {
            for (auto it = pool.available.end(); it != pool.available.begin(); ) {
                --it;
                if ((*it)->getActualState() != IVideo::VideoState::None) {
                    IVideo* key = it->get();
                    auto vid = std::move(*it);
                    pool.available.erase(it);
                    pool.index.erase(key);
                    pool.currentActive++;
                    LOG_DEBUG("VideoPool", "Acquire (HOT SWAP reuse) " + poolStateStr(monitor, listId, pool));
                    return vid;
                }
            }
        }

        // 2. THE COLD SWAP: Fallback to scanning for fully idle (None) pipelines
        for (auto it = pool.available.begin(); it != pool.available.end(); ++it) {
            if ((*it)->getActualState() == IVideo::VideoState::None) {
                IVideo* key = it->get();
                auto vid = std::move(*it);
                pool.available.erase(it);
                pool.index.erase(key);
                pool.currentActive++;
                LOG_DEBUG("VideoPool", "Acquire (COLD reuse) " + poolStateStr(monitor, listId, pool));
                return vid;
            }
        }

        return nullptr;
        };

    // PRE-LATCH: always create (records peak)
    if (!pool.initialCountLatched) {
        pool.currentActive++;
        pool.observedMaxActive = std::max(pool.observedMaxActive, pool.currentActive);
        lk.unlock();

        auto vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) {
            pool.currentActive--;
            LOG_WARNING("VideoPool", "Acquire (PreLatch create FAIL) " + poolStateStr(monitor, listId, pool));
            s_cv.notify_all();
            lk.lock();
        }
        else {
            LOG_DEBUG("VideoPool", "Acquire (PreLatch create OK) " + poolStateStr(monitor, listId, pool));
            vid->setSoftOverlay(softOverlay);
            return vid;
        }
    }

    // Post-latch loop: block until we can return a video
    for (;;) {
        if (shuttingDown_ || pool.markedForCleanup) {
            LOG_DEBUG("VideoPool", "Acquire (bail: shutdown/cleanup) " + poolStateStr(monitor, listId, pool));
            return nullptr;
        }

        // 1) any ready?
        if (auto vid = try_pop_reuse()) {
            LOG_DEBUG("VideoPool", "Acquire (reuse OK) " + poolStateStr(monitor, listId, pool));
            lk.unlock();
            vid->setSoftOverlay(softOverlay);
            return vid;
        }

        // 2) allowed to grow?
        const size_t total = pool.currentActive + pool.available.size();
        if (total < pool.requiredInstanceCount) {
            pool.currentActive++;
            lk.unlock();

            auto vid = std::make_unique<GStreamerVideo>(monitor);
            if (!vid || vid->hasError()) {
                pool.currentActive--;
                s_cv.notify_all();
                lk.lock();
            }
            else {
                LOG_DEBUG("VideoPool", "Acquire (Growth create OK) " + poolStateStr(monitor, listId, pool));
                vid->setSoftOverlay(softOverlay);
                return vid;
            }
        }

        // 3. MAX CAPACITY HOT SWAP: 
        // We are at max capacity and no pipelines are fully torn down. 
        // Grab the most recently released active instance.
        if (!pool.available.empty()) {
            auto it = std::prev(pool.available.end());
            IVideo* key = it->get();
            auto vid = std::move(*it);

            pool.available.erase(it);
            pool.index.erase(key);
            pool.currentActive++;

            LOG_DEBUG("VideoPool", "Acquire (HOT SWAP Max Cap) " + poolStateStr(monitor, listId, pool));
            lk.unlock();

            vid->setSoftOverlay(softOverlay);
            return vid;
        }

        LOG_DEBUG("VideoPool", "Acquire (wait) " + poolStateStr(monitor, listId, pool));

        // 4) Wait until an idle reaches None, cleanup/shutdown, or growth becomes possible
        using namespace std::chrono_literals;

        s_cv.wait_for(lk, 50ms, [&pool] {
            if (shuttingDown_ || pool.markedForCleanup) return true;

            if (std::any_of(pool.available.begin(), pool.available.end(),
                [](const VideoPtr& v) { return v && v->getActualState() == IVideo::VideoState::None; })) return true;

            const size_t totalNow = pool.currentActive + pool.available.size();
            return totalNow < pool.requiredInstanceCount;
            });
    }
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid) {
        LOG_DEBUG("VideoPool", "Release (ignored: null VideoPtr) Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
        return;
    }

    if (listId == -1 || shuttingDown_) {
        LOG_DEBUG("VideoPool", "Release (ignored: non-pooled or shutting down) Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
        return;
    }

    // --- THE FIX: Unload BEFORE handing ownership back to the pool ---
    // We own the unique_ptr exclusively right now, so it is mathematically 
    // impossible for cleanup() to destroy it while we are unloading it.
    if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
        gsv->unload();
    }

    // --- NOW safely lock and hand it back to the pool ---
    std::unique_lock<std::mutex> lk(s_mutex);
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) {
        LOG_DEBUG("VideoPool", "Release (no pool for monitor) Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
        return;
    }
    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) {
        LOG_DEBUG("VideoPool", "Release (no pool for listId) Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
        return;
    }

    PoolInfo& pool = lit->second;

    if (pool.currentActive > 0) {
        pool.currentActive--;
    }
    else {
        LOG_WARNING("VideoPool", "Release (currentActive already 0) " + poolStateStr(monitor, listId, pool));
    }

    IVideo* key = vid.get();

    // Ownership is transferred here!
    auto it = pool.available.insert(pool.available.end(), std::move(vid));
    pool.index[key] = it;

    if (!pool.initialCountLatched) {
        const size_t candidate = pool.observedMaxActive + POOL_BUFFER_INSTANCES;

        if (candidate > pool.requiredInstanceCount) {
            pool.requiredInstanceCount = candidate;
        }

        pool.initialCountLatched = true;

        LOG_DEBUG("VideoPool", "Release (latch required=" + std::to_string(pool.requiredInstanceCount) + ") " + poolStateStr(monitor, listId, pool));
    }
    else {
        LOG_DEBUG("VideoPool", "Release (reuse) " + poolStateStr(monitor, listId, pool));
    }

    const bool tryErase = pool.markedForCleanup && pool.currentActive == 0;

    lk.unlock();

    // Do the heavier erasure work without holding the pool mutex.
    if (tryErase) {
        std::scoped_lock<std::mutex> relk(s_mutex);
        auto mit2 = pools_.find(monitor);
        if (mit2 != pools_.end()) {
            auto lit2 = mit2->second.find(listId);
            if (lit2 != mit2->second.end()) {
                lit2->second.available.clear();
                lit2->second.index.clear();

                LOG_DEBUG("VideoPool", "Release (cleanup erase) Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));

                mit2->second.erase(lit2);
                if (mit2->second.empty()) {
                    pools_.erase(mit2);
                }
            }
        }
    }

    s_cv.notify_all();
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
    if (videos.empty() || listId == -1 || shuttingDown_) return;
    for (auto& vid : videos) {
        releaseVideo(std::move(vid), monitor, listId);
    }
}

void VideoPool::cleanup(int monitor, int listId) {
    std::scoped_lock<std::mutex> lock(s_mutex);

    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;
    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    PoolInfo& pool = lit->second;
    pool.markedForCleanup = true;

    erasePoolIfIdle_nolock(monitor, listId);
    s_cv.notify_all();

    LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
    LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
    shuttingDown_ = true;

    {
        std::scoped_lock<std::mutex> lock(s_mutex);

        for (auto& [monitor, listMap] : pools_) {
            for (auto& [listId, pool] : listMap) {
                pool.markedForCleanup = true;
            }
        }
        s_cv.notify_all();

        // Since GStreamerVideo now cleans up flawlessly in its destructor via stop(),
        // we can just violently clear the map.
        pools_.clear();
    }

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
    pool.index.clear();

    mit->second.erase(lit);
    if (mit->second.empty()) pools_.erase(mit);
}

inline std::string VideoPool::poolStateStr(int monitor, int listId, const VideoPool::PoolInfo& p) {
    return "Mon:" + std::to_string(monitor) +
        " List:" + std::to_string(listId) +
        " Active=" + std::to_string(p.currentActive) +
        " Avail=" + std::to_string(p.available.size()) +
        " Req=" + std::to_string(p.requiredInstanceCount) +
        (p.initialCountLatched ? " LATCHED" : " PRELATCH") +
        (p.markedForCleanup ? " CLEANUP" : "");
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (listId == -1 || shuttingDown_) return;

    std::scoped_lock<std::mutex> lk(s_mutex);
    PoolInfo& pool = pools_[monitor][listId];

    if (desiredTotal > pool.requiredInstanceCount) {
        pool.requiredInstanceCount = desiredTotal;
    }
}