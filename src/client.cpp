#include "client.h"

#include "socket_utils.h"

const int CLIENT_TIMEOUT_MS = 500;

// Keys go into a space-separated protocol, so they can't contain spaces or newlines.
static bool is_valid_key(const std::string& key) {
    return !key.empty() && key.find(' ') == std::string::npos &&
           key.find('\n') == std::string::npos && key.find('\r') == std::string::npos;
}

CacheClient::CacheClient(const ClusterConfig& config)
    : config(config), ring(config.virtual_nodes) {
    // Same ring as the servers (same config, same hash), so the client
    // computes the same owners for every key.
    for (size_t i = 0; i < config.nodes.size(); i++) {
        ring.add_node(config.nodes[i].id);
    }
}

bool CacheClient::put(const std::string& key, const std::string& value, long long ttl_ms) {
    if (!is_valid_key(key) || value.find('\n') != std::string::npos) {
        return false;
    }
    std::string reply;
    std::string served_by;
    std::string request = "PUT " + key + " " + std::to_string(ttl_ms) + " " + value;
    return send_to_owners(key, request, reply, served_by) && reply == "OK";
}

bool CacheClient::get(const std::string& key, std::string& out_value, std::string& served_by) {
    served_by = "";
    if (!is_valid_key(key)) {
        return false;
    }
    std::string reply;
    if (!send_to_owners(key, "GET " + key, reply, served_by)) {
        return false;
    }
    if (reply.compare(0, 6, "VALUE ") == 0) {
        out_value = reply.substr(6);
        return true;
    }
    return false;   // NOT_FOUND or an error
}

bool CacheClient::remove(const std::string& key) {
    if (!is_valid_key(key)) {
        return false;
    }
    std::string reply;
    std::string served_by;
    return send_to_owners(key, "DEL " + key, reply, served_by) && reply == "OK";
}

bool CacheClient::send_to_node(const std::string& node_id, const std::string& request,
                               std::string& reply) {
    NodeInfo node;
    if (!find_node(config, node_id, node)) {
        return false;
    }
    return send_request(node.host, node.port, request, CLIENT_TIMEOUT_MS, reply);
}

bool CacheClient::get_node_status(std::string& reply) {
    for (size_t i = 0; i < config.nodes.size(); i++) {
        if (send_to_node(config.nodes[i].id, "NODES", reply)) {
            return true;
        }
    }
    return false;
}

// Owners of the key (primary first), with nodes known to be DEAD moved to the end.
std::vector<std::string> CacheClient::nodes_for_key(const std::string& key) {
    std::vector<std::string> owners = ring.get_nodes(key, config.replication);
    std::vector<std::string> alive_first;
    std::vector<std::string> dead_last;

    std::lock_guard<std::mutex> lock(status_mtx);
    for (size_t i = 0; i < owners.size(); i++) {
        if (dead[owners[i]]) {
            dead_last.push_back(owners[i]);   // still tried, in case our info is old
        } else {
            alive_first.push_back(owners[i]);
        }
    }
    for (size_t i = 0; i < dead_last.size(); i++) {
        alive_first.push_back(dead_last[i]);
    }
    return alive_first;
}

bool CacheClient::send_to_owners(const std::string& key, const std::string& request,
                                 std::string& reply, std::string& served_by) {
    std::vector<std::string> order = nodes_for_key(key);
    for (size_t i = 0; i < order.size(); i++) {
        if (send_to_node(order[i], request, reply)) {
            served_by = order[i];
            return true;
        }
        // Failover: this node didn't answer (connect failed or timed out).
        // Refresh our view of which nodes are dead, then try the next owner.
        refresh_node_status();
    }
    return false;
}

void CacheClient::refresh_node_status() {
    std::string reply;
    if (!get_node_status(reply)) {
        return;
    }
    // Reply looks like: NODES n1=ALIVE n2=DEAD n3=ALIVE
    std::vector<std::string> parts = split(reply, 1000);
    std::lock_guard<std::mutex> lock(status_mtx);
    for (size_t i = 1; i < parts.size(); i++) {
        size_t eq = parts[i].find('=');
        if (eq != std::string::npos) {
            dead[parts[i].substr(0, eq)] = (parts[i].substr(eq + 1) == "DEAD");
        }
    }
}
