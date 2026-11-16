#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// One entry in the cache. Entries form a doubly linked list ordered by use.
struct Node {
    std::string key;
    std::string value;
    long long expire_at_ms;   // 0 means "never expires"
    long long version;        // used later for replication
    Node* prev;
    Node* next;
};

// Thread-safe LRU cache with optional per-key TTL.
// get, put and evict are all O(1): the hash map finds a node,
// and the linked list keeps the use order.
class LRUCache {
public:
    explicit LRUCache(int capacity);
    ~LRUCache();   // stops the cleanup thread, so it never outlives the cache

    bool get(const std::string& key, std::string& out_value);
    void put(const std::string& key, const std::string& value, long long ttl_ms);
    bool remove(const std::string& key);
    int size();

    // Replication versions: a write is applied only if its version is newer
    // than what we already store. Returns true if the cache was changed.
    bool put_with_version(const std::string& key, const std::string& value,
                          long long ttl_ms, long long version);
    bool remove_with_version(const std::string& key, long long version);
    std::string stats_string();   // "hits=.. misses=.. evictions=.. expired=.. size=.. hit_rate=.."

    // Background expiry: every interval_ms, remove expired keys near the LRU end.
    void start_cleanup_thread(int interval_ms);
    void stop_cleanup_thread();

    // Test helper: checks that the linked list and the map agree.
    bool check_list_ok();

private:
    void unlink_node(Node* node);
    void add_to_front(Node* node);
    void evict_lru();
    long long now_ms();
    void store_locked(const std::string& key, const std::string& value,
                      long long ttl_ms, long long version);
    void cleanup_loop(int interval_ms);
    void remove_expired_from_tail(int max_checks);

    int capacity;
    Node head;   // dummy node: most recently used is head.next
    Node tail;   // dummy node: least recently used is tail.prev
    std::unordered_map<std::string, std::unique_ptr<Node>> node_map;  // map owns the nodes
    std::mutex mtx;
    long long hits = 0;
    long long misses = 0;
    long long evictions = 0;
    long long expired = 0;

    // Cleanup thread state
    std::thread cleanup_thread;
    std::atomic<bool> running{false};
    std::condition_variable cleanup_cv;   // lets stop wake the sleeping thread at once
};

#endif
