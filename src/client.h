#ifndef CLIENT_H
#define CLIENT_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "hash_ring.h"

// Sends each key to the node that owns it, with failover to the replica.
// Opens a new connection per request (a connection pool is future work).
class CacheClient {
public:
    explicit CacheClient(const ClusterConfig& config);

    bool put(const std::string& key, const std::string& value, long long ttl_ms);
    // served_by is set to the node that answered (empty if none could be reached).
    bool get(const std::string& key, std::string& out_value, std::string& served_by);
    bool remove(const std::string& key);

    // Sends one raw request to a specific node (used for STATS).
    bool send_to_node(const std::string& node_id, const std::string& request, std::string& reply);
    // Asks any reachable node for its NODES table.
    bool get_node_status(std::string& reply);

private:
    std::vector<std::string> nodes_for_key(const std::string& key);
    bool send_to_owners(const std::string& key, const std::string& request,
                        std::string& reply, std::string& served_by);
    void refresh_node_status();

    ClusterConfig config;
    HashRing ring;
    std::mutex status_mtx;
    std::map<std::string, bool> dead;   // nodes reported DEAD by the last NODES reply
};

#endif
