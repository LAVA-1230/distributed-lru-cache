#ifndef HASH_RING_H
#define HASH_RING_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

// FNV-1a 64-bit hash. Also used by the benchmark for the "hash % N" comparison.
unsigned long long fnv1a_hash(const std::string& s);

// Consistent hash ring. Each physical node is placed at many points
// ("virtual nodes") so keys are spread evenly.
class HashRing {
public:
    explicit HashRing(int virtual_nodes);
    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);
    std::string get_node(const std::string& key);   // "" if the ring is empty
    std::vector<std::string> get_nodes(const std::string& key, int count);  // distinct physical nodes

private:
    unsigned long long hash_string(const std::string& s);
    std::map<unsigned long long, std::string> ring;   // position -> node id
    int virtual_nodes;
    std::mutex mtx;
};

#endif
