#include <iostream>
#include <string>
#include <vector>

#include "client.h"
#include "config.h"

static void print_usage() {
    std::cerr << "usage: dcache_cli [--config file] <command>\n"
              << "  put <key> <value> [ttl_ms]\n"
              << "  get <key>\n"
              << "  del <key>\n"
              << "  stats <node_id>\n"
              << "  nodes" << std::endl;
}

static int run_command(CacheClient& client, const std::vector<std::string>& args) {
    const std::string& command = args[0];

    if (command == "put" && (args.size() == 3 || args.size() == 4)) {
        long long ttl_ms = 0;
        if (args.size() == 4 && !parse_number(args[3], ttl_ms)) {
            std::cerr << "ttl_ms must be a number" << std::endl;
            return 1;
        }
        bool ok = client.put(args[1], args[2], ttl_ms);
        std::cout << (ok ? "OK" : "ERROR: put failed") << std::endl;
        return ok ? 0 : 1;
    }
    if (command == "get" && args.size() == 2) {
        std::string value;
        std::string served_by;
        if (client.get(args[1], value, served_by)) {
            std::cout << value << "   (served by " << served_by << ")" << std::endl;
            return 0;
        }
        if (served_by.empty()) {
            std::cout << "ERROR: no node reachable" << std::endl;
        } else {
            std::cout << "NOT_FOUND   (answered by " << served_by << ")" << std::endl;
        }
        return 1;
    }
    if (command == "del" && args.size() == 2) {
        bool ok = client.remove(args[1]);
        std::cout << (ok ? "OK" : "NOT_FOUND") << std::endl;
        return ok ? 0 : 1;
    }
    if ((command == "stats" && args.size() == 2) || (command == "nodes" && args.size() == 1)) {
        std::string reply;
        bool ok = (command == "stats") ? client.send_to_node(args[1], "STATS", reply)
                                       : client.get_node_status(reply);
        std::cout << (ok ? reply : "ERROR: node not reachable") << std::endl;
        return ok ? 0 : 1;
    }
    print_usage();
    return 1;
}

int main(int argc, char* argv[]) {
    std::string config_path = "cluster.conf";
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
            i++;
        } else {
            args.push_back(arg);
        }
    }
    if (args.empty()) {
        print_usage();
        return 1;
    }

    ClusterConfig config;
    std::string error;
    if (!load_config(config_path, config, error)) {
        std::cerr << "config error: " << error << std::endl;
        return 1;
    }
    CacheClient client(config);
    return run_command(client, args);
}
