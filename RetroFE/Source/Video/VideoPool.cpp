#include "VideoPool.h"
#include "../Utility/Log.h"
#include "../Utility/ThreadPool.h"
#include "GlibLoop.h"

std::mutex VideoPool::s_registryMutex;
std::atomic<bool> VideoPool::shuttingDown_ = false;
VideoPool::PoolMap VideoPool::pools_;

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay, bool priority) {
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

    // 1. Try to find a ready instance from what we already have
    auto try_pop_ready = [&]() -> VideoPtr {
        // First check hints (videos that just stopped)
        while (!pool->readyHints.empty()) {
            IVideo* key = pool->readyHints.front();
            pool->readyHints.pop_front();
            auto it = pool->index.find(key);
            if (it == pool->index.end()) continue;

            auto vidIt = it->second;
            VideoPtr vid = std::move(*vidIt);
            pool->available.erase(vidIt);
            pool->index.erase(it);
            pool->hinted.erase(key);
            pool->currentActive++;
            return vid;
        }

        // Then scan the available list
        for (auto it = pool->available.begin(); it != pool->available.end(); ++it) {
            if ((*it)->getActualState() == IVideo::VideoState::None) {
                IVideo* key = it->get();
                VideoPtr vid = std::move(*it);
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

    // 2. Proactive Growth / Priority Loading
    const size_t total = pool->currentActive + pool->available.size() + pool->pendingCreation;

    // We grow if we are under capacity OR if this is a priority request and we don't have an instance yet
    bool needsImmediate = (priority && !vid);
    bool needsCushion = (total < pool->requiredInstanceCount) || (!pool->initialCountLatched && pool->pendingCreation == 0);

    if ((needsImmediate || needsCushion) && !shuttingDown_ && !pool->markedForCleanup) {
        pool->pendingCreation++;
        std::weak_ptr<PoolInfo> poolWeak = pool;

        auto task = [monitor, listId, poolWeak]() {
            auto newVid = std::make_unique<GStreamerVideo>(monitor);
            VideoPool::injectNewInstance((newVid && !newVid->hasError()) ? std::move(newVid) : nullptr, poolWeak, monitor, listId);
            };

        try {
            if (priority) {
                // High priority: jump to the front of the ThreadPool queue
                ThreadPool::getInstance().enqueueAtFront(std::move(task));
            }
            else {
                // Standard: run on a ThreadPool thread so many constructions can run in parallel
                ThreadPool::getInstance().enqueue(std::move(task));
            }
        }
        catch (...) {
            // ThreadPool has been stopped (app is shutting down); undo the pending counter
            pool->pendingCreation--;
        }
    }

    // 3. Finalize the instance we found
    if (vid) {
        vid->setSoftOverlay(softOverlay);
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) {
            gsv->disarmOnBecameNone();
            gsv->setOnReadyCallback([monitor, listId](IVideo* v) {
                VideoPool::signalReady(v, monitor, listId);
                });
        }
    }

    return vid;
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

    {
        std::unique_lock lk(pool->poolMutex);
        if (pool->index.count(vid) && !pool->hinted.count(vid)) {
            pool->readyHints.push_back(vid);
            pool->hinted.insert(vid);
        }
        pool->poolCv.notify_all();
    }

    // Bulletproofing: If this was the last video to stop, try to erase the pool now
    tryErasePool(monitor, listId, pool);
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
            pool->requiredInstanceCount = pool->observedMaxActive + 2;
            pool->initialCountLatched = true;
        }
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) gsv->armOnBecameNone();
    }

    if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) gsv->unload();

    tryErasePool(monitor, listId, pool);
}

void VideoPool::tryErasePool(int monitor, int listId, PoolInfoPtr pool) {
    std::unique_lock lk(pool->poolMutex);
    if (!pool->markedForCleanup || pool->currentActive != 0) return;

    // We can only erase the pool if ALL available videos have finished unloading (state is None)
    for (const auto& v : pool->available) {
        if (v->getActualState() != IVideo::VideoState::None) return;
    }

    // Everyone is stopped and we are marked for cleanup�safe to delete from registry
    std::scoped_lock lock(s_registryMutex);
    auto mit = pools_.find(monitor);
    if (mit != pools_.end()) {
        mit->second.erase(listId);
    }
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
    for (auto& vid : videos) releaseVideo(std::move(vid), monitor, listId);
}

void VideoPool::cleanup(int monitor, int listId) {
    PoolInfoPtr pool;
    {
        std::scoped_lock lock(s_registryMutex);
        auto mit = pools_.find(monitor);
        if (mit == pools_.end()) return;
        auto lit = mit->second.find(listId);
        if (lit == mit->second.end()) return;
        pool = lit->second;
    }

    std::scoped_lock plock(pool->poolMutex);
    pool->markedForCleanup = true;
    // We don't erase here anymore�tryErasePool handles it once the GStreamer threads settle
}

void VideoPool::shutdown() {
    shuttingDown_ = true;
    std::scoped_lock lock(s_registryMutex);
    pools_.clear();
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
    if (desiredTotal > pool->requiredInstanceCount) pool->requiredInstanceCount = desiredTotal;
}

void VideoPool::injectNewInstance(VideoPtr vid, std::weak_ptr<PoolInfo> poolWeak, int monitor, int listId) {
    auto pool = poolWeak.lock();
    if (!pool) return;

    std::unique_lock lk(pool->poolMutex);
    if (pool->pendingCreation > 0) pool->pendingCreation--;

    if (vid && !pool->markedForCleanup && !shuttingDown_) {
        IVideo* key = vid.get();

        // --- BULLETPROOFING: Insert at the FRONT ---
        // Fresh instances are ready to play immediately. Putting them at the front 
        // ensures the UI thread grabs them the next time it calls acquireVideo, 
        // jumping ahead of "dirty" instances that are still settling at the back.
        auto it = pool->available.insert(pool->available.begin(), std::move(vid));

        pool->index[key] = it;
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) {
            gsv->armOnBecameNone();
        }
    }
    pool->poolCv.notify_all();
}

inline std::string VideoPool::poolStateStr(int monitor, int listId, const PoolInfo& p) {
    return "Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId) + " Active=" + std::to_string(p.currentActive);
}