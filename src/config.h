#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>

struct NodeInfo {
    std::string id;
    std::string host;
    int port;
};

struct ClusterConfig {
    int replication = 2;
    int virtual_nodes = 100;
    std::vector<NodeInfo> nodes;
};

// Reads a file like:
//   replication 2
//   virtual_nodes 100
//   node n1 127.0.0.1 7001
// Lines starting with '#' and empty lines are ignored.
bool load_config(const std::string& path, ClusterConfig& config, std::string& error);

// Finds a node by id. Returns false if it isn't in the config.
bool find_node(const ClusterConfig& config, const std::string& id, NodeInfo& out);

// Parses a whole string as a number. Returns false if it isn't one.
// (Used instead of std::stoll so bad input never throws.)
bool parse_number(const std::string& text, long long& out);

#endif
