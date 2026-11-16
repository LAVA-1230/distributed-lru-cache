#include "hash_ring.h"

unsigned long long fnv1a_hash(const std::string& s) {
    // FNV-1a instead of std::hash: std::hash can give different results with
    // different compilers or library versions, but every node and every client
    // must agree on where a key lives.
    unsigned long long hash = 14695981039346656037ULL;   // FNV offset basis
    for (size_t i = 0; i < s.size(); i++) {
        hash ^= (unsigned char)s[i];
        hash *= 1099511628211ULL;                         // FNV prime
    }
    // Final mixing step (the "fmix64" finalizer from MurmurHash3).
    // Plain FNV-1a barely changes the HIGH bits when only the last characters
    // differ ("key:1" vs "key:2", "n1#7" vs "n1#8"). The ring is sorted by the
    // whole number, so those points clumped together and one node got ~75% of
    // the keys. These shifts and multiplies spread every input bit over all 64.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

HashRing::HashRing(int virtual_nodes) : virtual_nodes(virtual_nodes) {
}

unsigned long long HashRing::hash_string(const std::string& s) {
    return fnv1a_hash(s);
}

void HashRing::add_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mtx);
    // Many positions per node (virtual nodes) so each node owns many small
    // slices of the ring instead of one big one: this spreads keys evenly.
    for (int i = 0; i < virtual_nodes; i++) {
        ring[hash_string(node_id + "#" + std::to_string(i))] = node_id;
    }
}

void HashRing::remove_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int i = 0; i < virtual_nodes; i++) {
        auto it = ring.find(hash_string(node_id + "#" + std::to_string(i)));
        if (it != ring.end() && it->second == node_id) {
            ring.erase(it);
        }
    }
}

std::string HashRing::get_node(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (ring.empty()) {
        return "";
    }
    // First position clockwise from the key; wrap around past the end.
    auto it = ring.lower_bound(hash_string(key));
    if (it == ring.end()) {
        it = ring.begin();
    }
    return it->second;
}

std::vector<std::string> HashRing::get_nodes(const std::string& key, int count) {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> result;
    if (ring.empty()) {
        return result;
    }

    auto it = ring.lower_bound(hash_string(key));
    // Walk clockwise at most one full lap.
    for (size_t steps = 0; steps < ring.size() && (int)result.size() < count; steps++) {
        if (it == ring.end()) {
            it = ring.begin();
        }
        // Skip a node we already have: two virtual nodes of the same server
        // must not hold both copies of a key.
        bool already_have = false;
        for (size_t j = 0; j < result.size(); j++) {
            if (result[j] == it->second) {
                already_have = true;
            }
        }
        if (!already_have) {
            result.push_back(it->second);
        }
        ++it;
    }
    return result;
}
