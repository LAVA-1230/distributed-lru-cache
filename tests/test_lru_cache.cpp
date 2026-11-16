#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lru_cache.h"

TEST(LRUCacheTest, PutAndGet) {
    LRUCache cache(10);
    std::string value;

    cache.put("a", "1", 0);
    EXPECT_TRUE(cache.get("a", value));
    EXPECT_EQ(value, "1");

    // Overwrite the same key
    cache.put("a", "2", 0);
    EXPECT_TRUE(cache.get("a", value));
    EXPECT_EQ(value, "2");
    EXPECT_EQ(cache.size(), 1);
    EXPECT_TRUE(cache.check_list_ok());
}

TEST(LRUCacheTest, EvictionOrder) {
    LRUCache cache(3);
    std::string value;

    cache.put("a", "1", 0);
    cache.put("b", "2", 0);
    cache.put("c", "3", 0);
    EXPECT_TRUE(cache.get("a", value));   // a becomes most recently used, b is now LRU
    cache.put("d", "4", 0);               // cache is full, so b is evicted

    EXPECT_FALSE(cache.get("b", value));
    EXPECT_TRUE(cache.get("a", value));
    EXPECT_TRUE(cache.get("c", value));
    EXPECT_TRUE(cache.get("d", value));
    EXPECT_EQ(cache.size(), 3);
    EXPECT_TRUE(cache.check_list_ok());
}

TEST(LRUCacheTest, CapacityOne) {
    LRUCache cache(1);
    std::string value;

    cache.put("a", "1", 0);
    cache.put("b", "2", 0);
    EXPECT_FALSE(cache.get("a", value));
    EXPECT_TRUE(cache.get("b", value));
    EXPECT_EQ(value, "2");
    EXPECT_EQ(cache.size(), 1);
    EXPECT_TRUE(cache.check_list_ok());
}

TEST(LRUCacheTest, RemoveMissingKey) {
    LRUCache cache(5);
    EXPECT_FALSE(cache.remove("nothing"));

    cache.put("a", "1", 0);
    EXPECT_TRUE(cache.remove("a"));
    EXPECT_FALSE(cache.remove("a"));
    EXPECT_EQ(cache.size(), 0);
    EXPECT_TRUE(cache.check_list_ok());
}

TEST(LRUCacheTest, StatsCounts) {
    LRUCache cache(2);
    std::string value;

    cache.put("a", "1", 0);
    cache.put("b", "2", 0);
    cache.get("a", value);    // hit
    cache.get("x", value);    // miss
    cache.put("c", "3", 0);   // evicts b
    cache.get("b", value);    // miss
    cache.get("c", value);    // hit

    EXPECT_EQ(cache.stats_string(),
              "hits=2 misses=2 evictions=1 expired=0 size=2 hit_rate=0.50");
}

TEST(LRUCacheTest, TtlExpiry) {
    LRUCache cache(5);
    std::string value;

    cache.put("temp", "x", 100);
    EXPECT_TRUE(cache.get("temp", value));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(cache.get("temp", value));
    EXPECT_EQ(cache.size(), 0);

    std::string stats = cache.stats_string();
    EXPECT_NE(stats.find("expired=1"), std::string::npos);
}

TEST(LRUCacheTest, CleanupThreadRemovesExpiredKeys) {
    LRUCache cache(100);
    cache.start_cleanup_thread(50);

    for (int i = 0; i < 10; i++) {
        cache.put("key" + std::to_string(i), "v", 100);
    }
    EXPECT_EQ(cache.size(), 10);

    // Never read the keys: only the background thread can remove them.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(cache.size(), 0);
    EXPECT_NE(cache.stats_string().find("expired=10"), std::string::npos);
    EXPECT_TRUE(cache.check_list_ok());

    cache.stop_cleanup_thread();
}

TEST(LRUCacheTest, CleanupThreadKeepsUnexpiredKeys) {
    LRUCache cache(100);
    cache.start_cleanup_thread(20);

    cache.put("forever", "v", 0);      // no TTL
    cache.put("long", "v", 60000);     // expires in 1 minute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(cache.size(), 2);
    cache.stop_cleanup_thread();
}

TEST(LRUCacheTest, StopCleanupThreadIsFast) {
    LRUCache cache(10);
    cache.start_cleanup_thread(10000);   // would sleep 10 s if stop didn't wake it

    auto start = std::chrono::steady_clock::now();
    cache.stop_cleanup_thread();
    auto elapsed = std::chrono::steady_clock::now() - start;

    long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(elapsed_ms, 500);
}

TEST(LRUCacheTest, DestructorStopsCleanupThread) {
    // If the destructor didn't join the thread, the program would crash here.
    LRUCache* cache = new LRUCache(10);
    cache->start_cleanup_thread(10);
    cache->put("a", "1", 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    delete cache;
    SUCCEED();
}

// Each worker thread does 10,000 random put/get calls on the shared cache.
static void random_worker(LRUCache* cache, int seed) {
    std::mt19937 rng(seed);
    std::string value;
    for (int i = 0; i < 10000; i++) {
        std::string key = "k" + std::to_string(rng() % 500);
        if (rng() % 2 == 0) {
            cache->put(key, "v", 0);
        } else {
            cache->get(key, value);
        }
    }
}

TEST(LRUCacheTest, MultiThreaded) {
    const int capacity = 100;
    LRUCache cache(capacity);

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.push_back(std::thread(random_worker, &cache, t));
    }
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }

    EXPECT_LE(cache.size(), capacity);
    EXPECT_TRUE(cache.check_list_ok());
}

// Puts keys with very short TTLs, so the cleanup thread has work to do.
static void short_ttl_worker(LRUCache* cache, int seed) {
    std::mt19937 rng(seed);
    std::string value;
    for (int i = 0; i < 5000; i++) {
        std::string key = "k" + std::to_string(rng() % 200);
        cache->put(key, "v", 1 + rng() % 5);
        cache->get(key, value);
    }
}

TEST(LRUCacheTest, CleanupThreadWithConcurrentWriters) {
    LRUCache cache(100);
    cache.start_cleanup_thread(1);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.push_back(std::thread(short_ttl_worker, &cache, t));
    }
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }

    cache.stop_cleanup_thread();
    EXPECT_LE(cache.size(), 100);
    EXPECT_TRUE(cache.check_list_ok());
}

TEST(LRUCacheTest, VersionedPutIgnoresOlderWrites) {
    LRUCache cache(10);
    std::string value;

    EXPECT_TRUE(cache.put_with_version("k", "new", 0, 200));
    EXPECT_FALSE(cache.put_with_version("k", "old", 0, 100));   // arrives late
    EXPECT_TRUE(cache.get("k", value));
    EXPECT_EQ(value, "new");

    EXPECT_TRUE(cache.put_with_version("k", "newer", 0, 300));
    EXPECT_TRUE(cache.get("k", value));
    EXPECT_EQ(value, "newer");
}

TEST(LRUCacheTest, VersionedRemoveIgnoresOlderDeletes) {
    LRUCache cache(10);
    cache.put_with_version("k", "v", 0, 200);
    EXPECT_FALSE(cache.remove_with_version("k", 100));   // older delete
    EXPECT_EQ(cache.size(), 1);
    EXPECT_TRUE(cache.remove_with_version("k", 300));
    EXPECT_EQ(cache.size(), 0);
}
