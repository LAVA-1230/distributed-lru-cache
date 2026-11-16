#include "thread_pool.h"

ThreadPool::ThreadPool(int num_threads) : stopping(false) {
    for (int i = 0; i < num_threads; i++) {
        workers.push_back(std::thread(&ThreadPool::worker_loop, this));
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (stopping) {
            return;   // pool is shutting down: no new work
        }
        tasks.push(task);
    }
    cv.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        stopping = true;
    }
    cv.notify_all();
    // Every thread that is started must be joined.
    for (size_t i = 0; i < workers.size(); i++) {
        if (workers[i].joinable()) {
            workers[i].join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx);
            // Wait with a condition, not a plain wait(): threads can wake up
            // without a notify (spurious wake-ups), so re-check the condition.
            cv.wait(lock, [this]() { return stopping || !tasks.empty(); });
            if (stopping && tasks.empty()) {
                return;
            }
            task = tasks.front();
            tasks.pop();
        }
        // Run the task without holding the lock, so other workers can take tasks.
        task();
    }
}
