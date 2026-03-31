#include "VideoPool.h"
#include "../Utility/Log.h"
#include "../Utility/ThreadPool.h"

std::mutex VideoPool::s_registryMutex;
std::atomic<bool> VideoPool::shuttingDown_ = false;
VideoPool::PoolMap VideoPool::pools_;

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        auto vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) return nullptr;
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    PoolInfoPtr pool;
    {
        std::scoped_lock lock(s_registryMutex);
        auto& p = pools_[monitor][listId];
        if (!p) p = std::make_shared<PoolInfo>();
        pool = p;
    }

    std::unique_lock lk(pool->poolMutex);
    if (shuttingDown_ || pool->markedForCleanup) return nullptr;

    // 1. Try to find a ready instance (hints first, then scan)
    auto try_pop_ready = [&]() -> VideoPtr {
        while (!pool->readyHints.empty()) {
            IVideo* key = pool->readyHints.front();
            pool->readyHints.pop_front();
            auto it = pool->index.find(key);
            if (it == pool->index.end()) continue;

            auto vidIt = it->second;
            auto vid = std::move(*vidIt);
            pool->available.erase(vidIt);
            pool->index.erase(it);
            pool->hinted.erase(key);
            pool->currentActive++;
            return vid;
        }

        for (auto it = pool->available.begin(); it != pool->available.end(); ++it) {
            if ((*it)->getActualState() == IVideo::VideoState::None) {
                IVideo* key = it->get();
                auto vid = std::move(*it);
                pool->available.erase(it);
                pool->index.erase(key);
                pool->hinted.erase(key);
                pool->currentActive++;
                return vid;
            }
        }
        return nullptr;
        };

    VideoPtr vid = try_pop_ready();

    // 2. Proactive Growth Logic
    const size_t total = pool->currentActive + pool->available.size() + pool->pendingCreation;
    bool shouldGrow = (total < pool->requiredInstanceCount) || (!pool->initialCountLatched && pool->pendingCreation == 0);

    if (shouldGrow && !shuttingDown_ && !pool->markedForCleanup) {
        pool->pendingCreation++;

        // Pass a weak_ptr to the background task to prevent memory leaks if the list is destroyed
        std::weak_ptr<PoolInfo> poolWeak = pool;

        ThreadPool::getInstance().enqueue([monitor, listId, poolWeak]() {
            auto newVid = std::make_unique<GStreamerVideo>(monitor);
            if (newVid && !newVid->hasError()) {
                VideoPool::injectNewInstance(std::move(newVid), poolWeak, monitor, listId);
            }
            else {
                VideoPool::injectNewInstance(nullptr, poolWeak, monitor, listId);
            }
            });
    }

    if (vid) {
        vid->setSoftOverlay(softOverlay);
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
            gsv->disarmOnBecameNone();
            // Link the video to this pool for immediate ready-signaling
            gsv->setOnReadyCallback([monitor, listId](IVideo* v) {
                VideoPool::signalReady(v, monitor, listId);
                });
        }
    }

    return vid;
}

void VideoPool::injectNewInstance(VideoPtr vid, std::weak_ptr<PoolInfo> poolWeak, int monitor, int listId) {
    auto pool = poolWeak.lock();
    if (!pool) return; // Pool was cleaned up while we were working

    std::unique_lock lk(pool->poolMutex);
    if (pool->pendingCreation > 0) pool->pendingCreation--;

    if (vid && !pool->markedForCleanup && !shuttingDown_) {
        IVideo* key = vid.get();
        auto it = pool->available.insert(pool->available.end(), std::move(vid));
        pool->index[key] = it;
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) gsv->armOnBecameNone();
        LOG_DEBUG("VideoPool", "Inject " + poolStateStr(monitor, listId, *pool));
    }
    pool->poolCv.notify_all();
}

void VideoPool::signalReady(IVideo* vid, int monitor, int listId) {
    PoolInfoPtr pool;
    {
        std::scoped_lock lock(s_registryMutex);
        auto mit = pools_.find(monitor);
        if (mit != pools_.end()) {
            auto lit = mit->second.find(listId);
            if (lit != mit->second.end()) pool = lit->second;
        }
    }

    if (!pool) return;

    std::unique_lock lk(pool->poolMutex);
    if (pool->index.count(vid) && !pool->hinted.count(vid)) {
        pool->readyHints.push_back(vid);
        pool->hinted.insert(vid);
    }
    pool->poolCv.notify_all();
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid || listId == -1 || shuttingDown_) return;

    PoolInfoPtr pool;
    {
        std::scoped_lock lock(s_registryMutex);
        auto mit = pools_.find(monitor);
        if (mit != pools_.end()) {
            auto lit = mit->second.find(listId);
            if (lit != mit->second.end()) pool = lit->second;
        }
    }

    if (!pool) return;

    IVideo* key = vid.get();
    {
        std::unique_lock lk(pool->poolMutex);
        if (pool->currentActive > 0) pool->currentActive--;

        auto it = pool->available.insert(pool->available.end(), std::move(vid));
        pool->index[key] = it;

        if (!pool->initialCountLatched) {
            pool->observedMaxActive = std::max(pool->observedMaxActive, pool->currentActive + 1);
            pool->requiredInstanceCount = pool->observedMaxActive + 2; // N + 2 cushion
            pool->initialCountLatched = true;
        }

        if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) gsv->armOnBecameNone();
    }

    // Unload is heavy; do it outside the pool lock
    if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) {
        gsv->unload();
    }

    // Final check for registry pruning
    {
        std::scoped_lock lock(s_registryMutex);
        if (pool->markedForCleanup && pool->currentActive == 0) {
            pools_[monitor].erase(listId);
        }
    }
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
    if (videos.empty() || listId == -1 || shuttingDown_) return;
    for (auto& vid : videos) {
        releaseVideo(std::move(vid), monitor, listId);
    }
}

void VideoPool::cleanup(int monitor, int listId) {
    std::scoped_lock lock(s_registryMutex);
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;
    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    auto pool = lit->second;
    std::scoped_lock plock(pool->poolMutex);
    pool->markedForCleanup = true;

    // If nothing is active, erase from map now. Otherwise, releaseVideo will erase it later.
    if (pool->currentActive == 0) {
        mit->second.erase(lit);
    }
}

void VideoPool::shutdown() {
    shuttingDown_ = true;
    std::scoped_lock lock(s_registryMutex);
    pools_.clear(); // shared_ptrs handle cleaning up the PoolInfo memory
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (listId == -1 || shuttingDown_) return;

    PoolInfoPtr pool;
    {
        std::scoped_lock lock(s_registryMutex);
        auto& p = pools_[monitor][listId];
        if (!p) p = std::make_shared<PoolInfo>();
        pool = p;
    }

    std::scoped_lock plock(pool->poolMutex);
    if (desiredTotal > pool->requiredInstanceCount) {
        pool->requiredInstanceCount = desiredTotal;
    }
}

inline std::string VideoPool::poolStateStr(int monitor, int listId, const PoolInfo& p) {
    return "Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId) +
        " Active=" + std::to_string(p.currentActive) + " Avail=" + std::to_string(p.available.size()) +
        " Pending=" + std::to_string(p.pendingCreation) + " Req=" + std::to_string(p.requiredInstanceCount);
}