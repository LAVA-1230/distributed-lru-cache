#include "server.h"

#include <chrono>
#include <iostream>

#include "socket_utils.h"

const int WORKER_THREADS = 16;
const int CLIENT_IDLE_TIMEOUT_MS = 30000;
const int FORWARD_TIMEOUT_MS = 500;
const int HEARTBEAT_INTERVAL_MS = 1000;
const int PING_TIMEOUT_MS = 300;
const int MAX_MISSED_PINGS = 3;
const int MAX_REPLICATION_QUEUE = 10000;
const int CLEANUP_INTERVAL_MS = 1000;

// One mutex for all log output, so lines from different threads don't mix.
static std::mutex log_mtx;

CacheServer::CacheServer(const std::string& my_id, const ClusterConfig& config, int cache_capacity)
    : my_id(my_id), config(config), me_found(false), cache(cache_capacity),
      ring(config.virtual_nodes), pool(WORKER_THREADS), listen_fd(-1),
      running(false), replication_failed(0), last_version(0) {
    for (size_t i = 0; i < config.nodes.size(); i++) {
        const NodeInfo& node = config.nodes[i];
        // Every node builds the same ring from the same config, so all nodes
        // and clients agree on which nodes own a key.
        ring.add_node(node.id);
        if (node.id == my_id) {
            me = node;
            me_found = true;
        } else {
            peers.push_back(node);
            replica_queues[node.id];   // creates the queue in place
            alive[node.id] = true;     // assume alive until pings say otherwise
            missed_pings[node.id] = 0;
        }
    }
}

CacheServer::~CacheServer() {
    stop();
}

bool CacheServer::start() {
    if (!me_found) {
        log("my id is not in the config");
        return false;
    }
    listen_fd = create_server_socket(me.port);
    if (listen_fd < 0) {
        return false;
    }
    running = true;
    cache.start_cleanup_thread(CLEANUP_INTERVAL_MS);
    accept_thread = std::thread(&CacheServer::accept_loop, this);
    for (size_t i = 0; i < peers.size(); i++) {
        ReplicaQueue* queue = &replica_queues[peers[i].id];
        queue->sender = std::thread(&CacheServer::replication_loop, this, peers[i], queue);
    }
    heartbeat_thread = std::thread(&CacheServer::heartbeat_loop, this);
    log("listening on port " + std::to_string(me.port));
    return true;
}

void CacheServer::stop() {
    // exchange returns the old value: only the first call does the work.
    if (!running.exchange(false)) {
        return;
    }
    // shutdown() is what reliably wakes a thread blocked in accept() on Linux;
    // close() alone may leave it blocked.
    shutdown_socket(listen_fd);
    close_socket(listen_fd);
    accept_thread.join();

    // Wake handlers that are blocked in recv() waiting for the next request.
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        for (size_t i = 0; i < active_clients.size(); i++) {
            shutdown_socket(active_clients[i]);
        }
    }

    // Take each lock before notifying, so a thread that is about to wait
    // can't miss the wake-up (it either sees running == false or gets notified).
    { std::lock_guard<std::mutex> lock(heartbeat_mtx); }
    heartbeat_cv.notify_all();
    heartbeat_thread.join();

    for (size_t i = 0; i < peers.size(); i++) {
        ReplicaQueue& queue = replica_queues[peers[i].id];
        { std::lock_guard<std::mutex> lock(queue.mtx); }
        queue.cv.notify_all();
        queue.sender.join();
    }

    pool.shutdown();
    cache.stop_cleanup_thread();
    log("stopped");
}

// ---------------- Connections ----------------

void CacheServer::accept_loop() {
    while (running) {
        int client_fd = accept_client(listen_fd);
        if (client_fd < 0) {
            continue;   // error, or stop() shut the socket: the loop re-checks running
        }
        if (!running) {
            close_socket(client_fd);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            active_clients.push_back(client_fd);
        }
        pool.submit([this, client_fd]() { handle_client(client_fd); });
    }
}

void CacheServer::handle_client(int client_fd) {
    // Idle timeout: a client that sends nothing for 30 s is disconnected,
    // so it can't hold a worker thread forever.
    set_timeout(client_fd, CLIENT_IDLE_TIMEOUT_MS);
    LineReader reader(client_fd);
    std::string line;

    while (running && reader.read_line(line)) {
        if (line.empty()) {
            continue;
        }
        std::string response = handle_command(line);
        if (!send_all(client_fd, response + "\n")) {
            break;
        }
    }
    if (reader.was_too_long()) {
        send_all(client_fd, "ERROR line too long\n");
    }

    // Remove from the list before closing, so stop() never calls shutdown()
    // on a closed fd number that the OS may already have reused.
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        for (size_t i = 0; i < active_clients.size(); i++) {
            if (active_clients[i] == client_fd) {
                active_clients.erase(active_clients.begin() + i);
                break;
            }
        }
    }
    close_socket(client_fd);
}

std::string CacheServer::handle_command(const std::string& line) {
    std::vector<std::string> parts = split(line, 2);
    if (parts.empty()) {
        return "ERROR empty command";   // a line of only spaces
    }
    std::string command = parts[0];

    if (command == "PING") {
        return "PONG " + my_id;
    } else if (command == "GET") {
        return handle_get(line, false);
    } else if (command == "LOCAL_GET") {
        return handle_get(line, true);
    } else if (command == "PUT") {
        return handle_put(line);
    } else if (command == "DEL") {
        return handle_del(line);
    } else if (command == "REPL_PUT") {
        return handle_repl_put(line);
    } else if (command == "REPL_DEL") {
        return handle_repl_del(line);
    } else if (command == "STATS") {
        return "STATS " + cache.stats_string() +
               " replication_failed=" + std::to_string(replication_failed.load());
    } else if (command == "NODES") {
        return "NODES " + nodes_string();
    }
    return "ERROR unknown command";
}

// ---------------- Commands ----------------

std::string CacheServer::handle_get(const std::string& line, bool local_only) {
    std::vector<std::string> parts = split(line, 3);
    if (parts.size() != 2) {
        return "ERROR usage: GET <key>";
    }
    const std::string& key = parts[1];

    // If this node doesn't own the key, ask a node that does.
    std::vector<std::string> owners = ring.get_nodes(key, config.replication);
    if (!local_only && !is_owner(owners)) {
        return forward_to_owners(owners, line);
    }

    std::string value;
    if (cache.get(key, value)) {
        return "VALUE " + value;
    }
    return "NOT_FOUND";
}

std::string CacheServer::handle_put(const std::string& line) {
    std::vector<std::string> parts = split(line, 4);
    long long ttl_ms = 0;
    if (parts.size() != 4 || !parse_number(parts[2], ttl_ms)) {
        return "ERROR usage: PUT <key> <ttl_ms> <value>";
    }
    const std::string& key = parts[1];
    const std::string& value = parts[3];

    std::vector<std::string> owners = ring.get_nodes(key, config.replication);
    if (!is_owner(owners)) {
        return forward_to_owners(owners, line);
    }

    // This node coordinates the write:
    // 1. pick a version  2. store locally  3. queue copies for replicas  4. reply
    long long version = next_version();
    cache.put_with_version(key, value, ttl_ms, version);
    queue_replication(owners, "REPL_PUT " + key + " " + std::to_string(version) + " " +
                                  std::to_string(ttl_ms) + " " + value);
    log("PUT " + key + " -> stored");
    return "OK";
}

std::string CacheServer::handle_del(const std::string& line) {
    std::vector<std::string> parts = split(line, 3);
    if (parts.size() != 2) {
        return "ERROR usage: DEL <key>";
    }
    const std::string& key = parts[1];

    std::vector<std::string> owners = ring.get_nodes(key, config.replication);
    if (!is_owner(owners)) {
        return forward_to_owners(owners, line);
    }

    long long version = next_version();
    bool found = cache.remove_with_version(key, version);
    // Replicate even if we didn't have it: the replica might.
    queue_replication(owners, "REPL_DEL " + key + " " + std::to_string(version));
    log("DEL " + key + (found ? " -> removed" : " -> not found"));
    return found ? "OK" : "NOT_FOUND";
}

std::string CacheServer::handle_repl_put(const std::string& line) {
    // REPL_PUT <key> <version> <ttl_ms> <value...>
    std::vector<std::string> parts = split(line, 5);
    long long version = 0;
    long long ttl_ms = 0;
    if (parts.size() != 5 || !parse_number(parts[2], version) || !parse_number(parts[3], ttl_ms)) {
        return "ERROR bad REPL_PUT";
    }
    // put_with_version ignores the write if we already have a newer version,
    // because replication messages can arrive out of order.
    bool stored = cache.put_with_version(parts[1], parts[4], ttl_ms, version);
    log("REPL_PUT " + parts[1] + (stored ? " -> stored" : " -> ignored (older version)"));
    return "OK";
}

std::string CacheServer::handle_repl_del(const std::string& line) {
    std::vector<std::string> parts = split(line, 4);
    long long version = 0;
    if (parts.size() != 3 || !parse_number(parts[2], version)) {
        return "ERROR bad REPL_DEL";
    }
    cache.remove_with_version(parts[1], version);
    return "OK";
}

// ---------------- Routing ----------------

bool CacheServer::is_owner(const std::vector<std::string>& owners) {
    for (size_t i = 0; i < owners.size(); i++) {
        if (owners[i] == my_id) {
            return true;
        }
    }
    return false;
}

std::string CacheServer::forward_to_owners(const std::vector<std::string>& owners,
                                           const std::string& line) {
    // Try owners that are alive first; dead ones last, in case our view is stale.
    std::vector<std::string> order;
    for (size_t i = 0; i < owners.size(); i++) {
        if (is_alive(owners[i])) {
            order.push_back(owners[i]);
        }
    }
    for (size_t i = 0; i < owners.size(); i++) {
        if (!is_alive(owners[i])) {
            order.push_back(owners[i]);
        }
    }

    for (size_t i = 0; i < order.size(); i++) {
        NodeInfo node;
        std::string reply;
        if (find_node(config, order[i], node) &&
            send_request(node.host, node.port, line, FORWARD_TIMEOUT_MS, reply)) {
            return reply;
        }
    }
    return "ERROR no owner node reachable";
}

long long CacheServer::next_version() {
    // Versions use wall-clock time (system_clock), not steady_clock, because
    // they are compared across different processes. (TTL uses steady_clock.)
    // Bumping by 1 keeps versions strictly increasing even within one ms.
    std::lock_guard<std::mutex> lock(version_mtx);
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now <= last_version) {
        now = last_version + 1;
    }
    last_version = now;
    return now;
}

// ---------------- Replication ----------------

void CacheServer::queue_replication(const std::vector<std::string>& owners,
                                    const std::string& message) {
    for (size_t i = 0; i < owners.size(); i++) {
        if (owners[i] == my_id) {
            continue;
        }
        auto it = replica_queues.find(owners[i]);
        if (it == replica_queues.end()) {
            continue;
        }
        ReplicaQueue& queue = it->second;
        {
            std::lock_guard<std::mutex> lock(queue.mtx);
            // Cap the queue so a dead peer can't make it grow without limit.
            if ((int)queue.messages.size() >= MAX_REPLICATION_QUEUE) {
                replication_failed++;
                continue;
            }
            queue.messages.push(message);
        }
        queue.cv.notify_one();
    }
}

void CacheServer::replication_loop(NodeInfo peer, ReplicaQueue* queue) {
    int fd = -1;   // one long-lived connection to this peer, reopened when needed
    while (true) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(queue->mtx);
            queue->cv.wait(lock, [this, queue]() { return !running || !queue->messages.empty(); });
            if (!running) {
                break;
            }
            message = queue->messages.front();
            queue->messages.pop();
        }
        // Asynchronous replication: the client already got "OK". If the peer
        // is down we drop the copy; it's a cache, the database still has the data.
        if (!is_alive(peer.id) || !send_replication(peer, fd, message)) {
            replication_failed++;
        }
    }
    close_socket(fd);
}

bool CacheServer::send_replication(const NodeInfo& peer, int& fd, const std::string& message) {
    // Two tries: the long-lived connection may have been closed by the peer
    // (idle timeout or restart), so on failure reconnect once and retry.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (fd < 0) {
            fd = connect_to(peer.host, peer.port, FORWARD_TIMEOUT_MS);
            if (fd < 0) {
                return false;
            }
        }
        std::string reply;
        LineReader reader(fd);
        if (send_all(fd, message + "\n") && reader.read_line(reply) && reply == "OK") {
            return true;
        }
        close_socket(fd);
        fd = -1;
    }
    return false;
}

// ---------------- Failure detection ----------------

void CacheServer::heartbeat_loop() {
    std::unique_lock<std::mutex> lock(heartbeat_mtx);
    while (running) {
        // wait_for instead of sleep, so stop() can wake us immediately.
        heartbeat_cv.wait_for(lock, std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS),
                              [this]() { return !running; });
        if (!running) {
            break;
        }
        lock.unlock();   // don't hold the lock during slow network calls
        for (size_t i = 0; i < peers.size(); i++) {
            ping_peer(peers[i]);
        }
        lock.lock();
    }
}

void CacheServer::ping_peer(const NodeInfo& peer) {
    std::string reply;
    bool ok = send_request(peer.host, peer.port, "PING", PING_TIMEOUT_MS, reply) &&
              reply.compare(0, 4, "PONG") == 0;

    std::lock_guard<std::mutex> lock(status_mtx);
    if (ok) {
        missed_pings[peer.id] = 0;
        if (!alive[peer.id]) {
            alive[peer.id] = true;
            log(peer.id + " is back ALIVE");
        }
        return;
    }
    // Several misses in a row, not just one, so a single slow reply
    // doesn't mark a healthy node as dead.
    missed_pings[peer.id]++;
    if (missed_pings[peer.id] >= MAX_MISSED_PINGS && alive[peer.id]) {
        alive[peer.id] = false;
        log(peer.id + " is DEAD");
    }
}

bool CacheServer::is_alive(const std::string& node_id) {
    if (node_id == my_id) {
        return true;
    }
    std::lock_guard<std::mutex> lock(status_mtx);
    auto it = alive.find(node_id);
    return it != alive.end() && it->second;
}

std::string CacheServer::nodes_string() {
    std::string result;
    for (size_t i = 0; i < config.nodes.size(); i++) {
        const std::string& id = config.nodes[i].id;
        if (i > 0) {
            result += " ";
        }
        result += id + (is_alive(id) ? "=ALIVE" : "=DEAD");
    }
    return result;
}

void CacheServer::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mtx);
    std::cout << "[" << my_id << "] " << message << std::endl;
}
