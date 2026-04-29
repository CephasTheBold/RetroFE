#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <deque>       // Changed from queue to deque
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>      // for std::future, std::packaged_task
#include <functional>  // for std::invoke
#include <stdexcept>
#include <type_traits> // for std::invoke_result_t, std::is_void_v
#include <utility>     // for std::move, std::forward
#include <memory>      // for std::shared_ptr
#include <tuple>       // for std::tuple, std::make_tuple, std::apply

#include "Log.h"

class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    void shutdown();

    // A thread pool is a unique resource. It should not be copied or moved.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Singleton accessor
    static ThreadPool& getInstance();

    void wait();

    // 1. The Priority-Aware Declaration
    template<class F, class... Args>
    auto enqueue(bool isPriority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    // 2. The Original Fallback Declaration
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

private:
    // Worker threads
    std::vector<std::thread> workers;

    // Task queue (Now a double-ended queue so priority tasks can jump the line)
    std::deque<std::function<void()>> tasks;

    // Synchronization
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
    std::condition_variable waitCondition;
    size_t activeWorkers = 0;
};

// ----------------------------------------------------------------------------
// Implementations
// ----------------------------------------------------------------------------

// 1. The Priority-Aware Implementation
template<class F, class... Args>
auto ThreadPool::enqueue(bool isPriority, F&& f, Args&&... args)
-> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    // Store callable and args by value (moved), invoke via std::apply
    auto pkg = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f),
        tup = std::make_tuple(std::forward<Args>(args)...)
        ]() mutable -> return_type {
            if constexpr (std::is_void_v<return_type>) {
                std::apply(std::move(func), std::move(tup));
                return;
            }
            else {
                return std::apply(std::move(func), std::move(tup));
            }
        }
    );

    std::future<return_type> res = pkg->get_future();
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");

        if (isPriority) {
            tasks.emplace_front([pkg] { (*pkg)(); });
        }
        else {
            tasks.emplace_back([pkg] { (*pkg)(); });
        }
    }
    condition.notify_one();
    return res;
}

// 2. The Original Fallback Implementation
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<std::invoke_result_t<F, Args...>> {
    // Pass false for priority and forward the rest to the main enqueue function
    return enqueue(false, std::forward<F>(f), std::forward<Args>(args)...);
}

#endif // THREADPOOL_H