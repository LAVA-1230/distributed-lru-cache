#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// A fixed number of worker threads that run tasks from a shared queue.
class ThreadPool {
public:
    explicit ThreadPool(int num_threads);
    ~ThreadPool();

    void submit(std::function<void()> task);
    // Stops the workers after the queued tasks are done, and joins them.
    void shutdown();

private:
    void worker_loop();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping;
};

#endif
