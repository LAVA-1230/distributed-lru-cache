#ifndef SERVER_H
#define SERVER_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "hash_ring.h"
#include "lru_cache.h"
#include "thread_pool.h"

// Messages waiting to be sent to one replica node, plus the thread that sends them.
struct ReplicaQueue {
    std::queue<std::string> messages;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread sender;
};

// One cache node: listens on its port, answers the text protocol,
// replicates writes to the replica node, and pings the other nodes.
class CacheServer {
public:
    CacheServer(const std::string& my_id, const ClusterConfig& config, int cache_capacity);
    ~CacheServer();

    bool start();   // false if the port can't be opened or my_id isn't in the config
    void stop();    // safe to call more than once

private:
    // Connections
    void accept_loop();
    void handle_client(int client_fd);
    std::string handle_command(const std::string& line);

    // Commands
    std::string handle_get(const std::string& line, bool local_only);
    std::string handle_put(const std::string& line);
    std::string handle_del(const std::string& line);
    std::string handle_repl_put(const std::string& line);
    std::string handle_repl_del(const std::string& line);

    // Routing
    bool is_owner(const std::vector<std::string>& owners);
    std::string forward_to_owners(const std::vector<std::string>& owners, const std::string& line);
    long long next_version();

    // Replication (Phase 7)
    void queue_replication(const std::vector<std::string>& owners, const std::string& message);
    void replication_loop(NodeInfo peer, ReplicaQueue* queue);
    bool send_replication(const NodeInfo& peer, int& fd, const std::string& message);

    // Failure detection (Phase 8)
    void heartbeat_loop();
    void ping_peer(const NodeInfo& peer);
    bool is_alive(const std::string& node_id);
    std::string nodes_string();

    void log(const std::string& message);

    std::string my_id;
    ClusterConfig config;
    NodeInfo me;
    bool me_found;
    std::vector<NodeInfo> peers;   // every node except me

    LRUCache cache;
    HashRing ring;
    ThreadPool pool;

    int listen_fd;
    std::atomic<bool> running;
    std::thread accept_thread;

    // Open client sockets, so stop() can wake handlers blocked in recv()
    std::mutex clients_mtx;
    std::vector<int> active_clients;

    // One queue + sender thread per peer. Filled in the constructor and never
    // changed afterwards, so threads can read the map without a lock.
    std::map<std::string, ReplicaQueue> replica_queues;
    std::atomic<long long> replication_failed;

    std::thread heartbeat_thread;
    std::mutex heartbeat_mtx;
    std::condition_variable heartbeat_cv;

    std::mutex status_mtx;   // protects missed_pings and alive
    std::map<std::string, int> missed_pings;
    std::map<std::string, bool> alive;

    std::mutex version_mtx;
    long long last_version;
};

#endif
