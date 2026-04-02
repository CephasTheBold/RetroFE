#pragma once

#include <list>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <memory>
#include <condition_variable>
#include "GStreamerVideo.h"

class VideoPool {
public:
    using VideoPtr = std::unique_ptr<IVideo>;

    static VideoPtr acquireVideo(int monitor, int listId, bool softOverlay, bool priority = false);
    static void releaseVideo(VideoPtr vid, int monitor, int listId);
    static void releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId);
    static void cleanup(int monitor, int listId);
    static void shutdown();
    static void reserveCapacity(int monitor, int listId, size_t desiredTotal);

    // Bulletproofing: Signal from GStreamer thread that an instance is now Ready/None
    static void signalReady(IVideo* vid, int monitor, int listId);

    static std::atomic<bool> shuttingDown_;

private:
    struct PoolInfo {
        mutable std::mutex poolMutex;
        std::condition_variable poolCv;

        std::list<VideoPtr> available;
        std::deque<IVideo*> readyHints;
        std::unordered_map<IVideo*, std::list<VideoPtr>::iterator> index;
        std::unordered_set<IVideo*> hinted;

        size_t currentActive = 0;
        size_t pendingCreation = 0;
        size_t observedMaxActive = 0;
        size_t requiredInstanceCount = 0;
        bool initialCountLatched = false;
        bool markedForCleanup = false;
    };

    static std::mutex s_registryMutex;

    using PoolInfoPtr = std::shared_ptr<PoolInfo>;
    using ListPoolMap = std::unordered_map<int, PoolInfoPtr>;
    using PoolMap = std::unordered_map<int, ListPoolMap>;

    static PoolMap pools_;

    // Helper to check if a pool is truly safe to remove from the registry
    static void tryErasePool(int monitor, int listId, PoolInfoPtr pool);
    static void injectNewInstance(VideoPtr vid, std::weak_ptr<PoolInfo> poolWeak, int monitor, int listId);
    static std::string poolStateStr(int monitor, int listId, const PoolInfo& p);
};