#include "lru_cache.h"

#include <chrono>
#include <cstdio>

LRUCache::LRUCache(int capacity) : capacity(capacity) {
    // Dummy head and tail nodes point at each other, so the list is never
    // "empty" in a way that needs null checks when inserting or removing.
    head.expire_at_ms = 0;
    head.version = 0;
    head.prev = nullptr;
    head.next = &tail;

    tail.expire_at_ms = 0;
    tail.version = 0;
    tail.prev = &head;
    tail.next = nullptr;
}

LRUCache::~LRUCache() {
    // The thread uses this object, so it must be joined before the members are
    // destroyed. Forgetting to join a std::thread calls std::terminate().
    stop_cleanup_thread();
}

void LRUCache::start_cleanup_thread(int interval_ms) {
    if (running) {
        return;   // already started
    }
    running = true;
    cleanup_thread = std::thread(&LRUCache::cleanup_loop, this, interval_ms);
}

void LRUCache::stop_cleanup_thread() {
    {
        // Set the flag while holding the lock. Otherwise the thread could check
        // the flag, then we notify, then it starts waiting and misses the wake-up.
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
    }
    cleanup_cv.notify_all();
    if (cleanup_thread.joinable()) {
        cleanup_thread.join();
    }
}

void LRUCache::cleanup_loop(int interval_ms) {
    std::unique_lock<std::mutex> lock(mtx);
    while (running) {
        // wait_for releases the mutex while sleeping, so get/put keep working.
        // The condition makes it return early when stop is called, and also
        // handles spurious wake-ups.
        cleanup_cv.wait_for(lock, std::chrono::milliseconds(interval_ms),
                            [this]() { return !running; });
        if (!running) {
            break;
        }
        // wait_for gives the mutex back to us, so we already hold the lock here.
        remove_expired_from_tail(50);
    }
}

// Walks up to max_checks nodes from the LRU end and removes expired ones.
// Only a limited number is checked so the lock is held for a short time and
// normal get/put calls are not blocked. Caller must hold the lock.
void LRUCache::remove_expired_from_tail(int max_checks) {
    long long now = now_ms();
    Node* current = tail.prev;
    int checked = 0;
    while (current != &head && checked < max_checks) {
        Node* before = current->prev;   // save it first: current may be freed below
        if (current->expire_at_ms != 0 && now >= current->expire_at_ms) {
            // Copy the key: erase() frees the node, and the key string lives inside it.
            std::string key = current->key;
            unlink_node(current);
            node_map.erase(key);   // frees current
            expired++;
        }
        current = before;
        checked++;
    }
}

bool LRUCache::get(const std::string& key, std::string& out_value) {
    // Lock is needed even for get, because get moves the node to the front
    // (it changes the list) and also updates the stats counters.
    std::lock_guard<std::mutex> lock(mtx);

    auto it = node_map.find(key);
    if (it == node_map.end()) {
        misses++;
        return false;
    }

    Node* node = it->second.get();

    // Lazy expiry: an expired key is removed when someone tries to read it.
    if (node->expire_at_ms != 0 && now_ms() >= node->expire_at_ms) {
        unlink_node(node);
        node_map.erase(it);   // frees the node (map owns it)
        expired++;
        misses++;
        return false;
    }

    unlink_node(node);
    add_to_front(node);
    hits++;
    out_value = node->value;
    return true;
}

void LRUCache::put(const std::string& key, const std::string& value, long long ttl_ms) {
    // Lock needed: put changes both the map and the list.
    std::lock_guard<std::mutex> lock(mtx);
    store_locked(key, value, ttl_ms, 0);
}

bool LRUCache::put_with_version(const std::string& key, const std::string& value,
                                long long ttl_ms, long long version) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = node_map.find(key);
    if (it != node_map.end()) {
        Node* node = it->second.get();
        bool is_expired = node->expire_at_ms != 0 && now_ms() >= node->expire_at_ms;
        // Replication messages can arrive out of order. Ignore a write that is
        // not newer than what we have, so an old value never replaces a new one.
        if (!is_expired && version <= node->version) {
            return false;
        }
    }
    store_locked(key, value, ttl_ms, version);
    return true;
}

bool LRUCache::remove_with_version(const std::string& key, long long version) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = node_map.find(key);
    if (it == node_map.end()) {
        return false;
    }
    // A newer PUT already arrived, so this older DEL must not remove it.
    if (it->second->version > version) {
        return false;
    }
    unlink_node(it->second.get());
    node_map.erase(it);
    return true;
}

// Inserts or updates a key and moves it to the front. Caller must hold the lock.
void LRUCache::store_locked(const std::string& key, const std::string& value,
                            long long ttl_ms, long long version) {
    long long expire_at = 0;
    if (ttl_ms > 0) {
        expire_at = now_ms() + ttl_ms;
    }

    auto it = node_map.find(key);
    if (it != node_map.end()) {
        // Key already exists: update it and mark it as most recently used.
        Node* node = it->second.get();
        node->value = value;
        node->expire_at_ms = expire_at;
        node->version = version;
        unlink_node(node);
        add_to_front(node);
        return;
    }

    // New key: make room first if the cache is full.
    if ((int)node_map.size() >= capacity) {
        evict_lru();
    }

    std::unique_ptr<Node> node(new Node());
    node->key = key;
    node->value = value;
    node->expire_at_ms = expire_at;
    node->version = version;
    node->prev = nullptr;
    node->next = nullptr;

    add_to_front(node.get());
    node_map[key] = std::move(node);
}

bool LRUCache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = node_map.find(key);
    if (it == node_map.end()) {
        return false;
    }
    unlink_node(it->second.get());
    node_map.erase(it);
    return true;
}

int LRUCache::size() {
    std::lock_guard<std::mutex> lock(mtx);
    return (int)node_map.size();
}

std::string LRUCache::stats_string() {
    std::lock_guard<std::mutex> lock(mtx);

    long long total = hits + misses;
    double hit_rate = 0.0;
    if (total > 0) {
        hit_rate = (double)hits / (double)total;
    }

    char rate_text[32];
    std::snprintf(rate_text, sizeof(rate_text), "%.2f", hit_rate);

    std::string result = "hits=" + std::to_string(hits) +
                         " misses=" + std::to_string(misses) +
                         " evictions=" + std::to_string(evictions) +
                         " expired=" + std::to_string(expired) +
                         " size=" + std::to_string(node_map.size()) +
                         " hit_rate=" + rate_text;
    return result;
}

bool LRUCache::check_list_ok() {
    std::lock_guard<std::mutex> lock(mtx);

    // Walk forward from head; every step must agree with the backward pointer,
    // and the number of real nodes must equal the map size.
    size_t count = 0;
    Node* current = &head;
    while (current->next != &tail) {
        Node* next = current->next;
        if (next == nullptr || next->prev != current) {
            return false;
        }
        current = next;
        count++;
        if (count > node_map.size()) {
            return false;   // more nodes in the list than in the map (or a loop)
        }
    }
    if (tail.prev != current) {
        return false;
    }
    return count == node_map.size();
}

// Removes a node from the list (does not free it).
// No null checks needed, because head and tail dummies are always there.
void LRUCache::unlink_node(Node* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

// Inserts a node right after the dummy head (most recently used position).
void LRUCache::add_to_front(Node* node) {
    node->prev = &head;
    node->next = head.next;
    head.next->prev = node;
    head.next = node;
}

// Removes the least recently used node. Caller must hold the lock.
void LRUCache::evict_lru() {
    Node* victim = tail.prev;
    if (victim == &head) {
        return;   // list is empty
    }
    // Copy the key: erase() frees the node, and the key string lives inside it.
    std::string key = victim->key;
    unlink_node(victim);
    node_map.erase(key);   // frees the node, so don't use victim after this
    evictions++;
}

long long LRUCache::now_ms() {
    // steady_clock, not system_clock: the system clock can jump (NTP sync,
    // user changing the time), which would make keys expire too early or late.
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}
