#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "config.h"
#include "server.h"

// Set by the signal handler. A handler may only do very simple things,
// so it just sets this flag and the main thread does the real shutdown.
static std::atomic<bool> stop_requested(false);

static void handle_signal(int) {
    stop_requested = true;
}

static void print_usage() {
    std::cerr << "usage: dcache_node --id <node_id> --config <file> [--capacity N]" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string node_id;
    std::string config_path = "cluster.conf";
    long long capacity = 10000;

    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i];
        std::string value = argv[i + 1];
        if (flag == "--id") {
            node_id = value;
        } else if (flag == "--config") {
            config_path = value;
        } else if (flag == "--capacity" && parse_number(value, capacity) && capacity > 0) {
            // parsed
        } else {
            print_usage();
            return 1;
        }
    }
    if (node_id.empty()) {
        print_usage();
        return 1;
    }

    ClusterConfig config;
    std::string error;
    if (!load_config(config_path, config, error)) {
        std::cerr << "config error: " << error << std::endl;
        return 1;
    }

    // Extra safety: never let a write to a closed socket kill the process.
    // (send_all already uses MSG_NOSIGNAL.)
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);    // Ctrl+C
    signal(SIGTERM, handle_signal);   // kill / stop_cluster.sh

    CacheServer server(node_id, config, (int)capacity);
    if (!server.start()) {
        std::cerr << "failed to start node " << node_id << std::endl;
        return 1;
    }

    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.stop();
    return 0;
}
