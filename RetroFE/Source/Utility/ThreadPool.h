#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>       // Reverted to standard queue
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>      
#include <functional>  
#include <stdexcept>
#include <type_traits> 
#include <utility>     
#include <memory>      
#include <tuple>       

#include "Log.h"

class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    void shutdown();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    static ThreadPool& getInstance();

    void wait();

    // Standard FIFO enqueue
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

private:
    std::vector<std::thread> workers;

    // Reverted to std::queue
    std::queue<std::function<void()>> tasks;

    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
    std::condition_variable waitCondition;
    size_t activeWorkers = 0;
};

// --- Implementation ---
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

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

        // Standard push to back of queue
        tasks.emplace([pkg] { (*pkg)(); });
    }
    condition.notify_one();
    return res;
}

#endif // THREADPOOL_H