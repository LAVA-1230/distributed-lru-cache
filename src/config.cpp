#include "config.h"

#include <fstream>
#include <sstream>

bool parse_number(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) {
        return false;   // empty, or too long to fit safely
    }
    long long value = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    out = value;
    return true;
}

// Reads one non-comment line into the config.
static bool parse_config_line(const std::string& line, ClusterConfig& config,
                              std::string& error) {
    std::istringstream in(line);
    std::string word;
    in >> word;

    if (word == "replication") {
        in >> config.replication;
    } else if (word == "virtual_nodes") {
        in >> config.virtual_nodes;
    } else if (word == "node") {
        NodeInfo node;
        in >> node.id >> node.host >> node.port;
        config.nodes.push_back(node);
    } else {
        error = "unknown setting: " + word;
        return false;
    }
    if (in.fail()) {
        error = "bad line: " + line;
        return false;
    }
    return true;
}

bool load_config(const std::string& path, ClusterConfig& config, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!parse_config_line(line, config, error)) {
            return false;
        }
    }
    if (config.nodes.empty()) {
        error = "no nodes in " + path;
        return false;
    }
    return true;
}

bool find_node(const ClusterConfig& config, const std::string& id, NodeInfo& out) {
    for (size_t i = 0; i < config.nodes.size(); i++) {
        if (config.nodes[i].id == id) {
            out = config.nodes[i];
            return true;
        }
    }
    return false;
}
