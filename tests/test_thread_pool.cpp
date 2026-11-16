#include <gtest/gtest.h>

#include <atomic>

#include "thread_pool.h"

TEST(ThreadPoolTest, RunsAllTasks) {
    std::atomic<int> counter(0);
    ThreadPool pool(4);
    for (int i = 0; i < 1000; i++) {
        pool.submit([&counter]() { counter++; });
    }
    pool.shutdown();   // waits for queued tasks, then joins the workers
    EXPECT_EQ(counter.load(), 1000);
}

TEST(ThreadPoolTest, ShutdownTwiceIsSafe) {
    ThreadPool pool(2);
    pool.shutdown();
    pool.shutdown();
    SUCCEED();
}
