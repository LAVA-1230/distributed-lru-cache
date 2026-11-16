# Distributed LRU Cache in C++ (Linux / WSL Ubuntu) — Build Plan for Claude Code

---

## ⚠️ CODING STYLE RULES — FOLLOW THESE IN EVERY FILE

This project should look like it was written by a **good student**, not a library author. The code must be easy to read and easy to explain line by line in an interview.

**Use only these:**
- C++17, built with **g++** + **CMake** on **Ubuntu (WSL2)**
- `std::string`, `std::vector`, `std::unordered_map`, `std::map`, `std::queue`
- `std::thread`, `std::mutex`, `std::lock_guard`, `std::unique_lock`, `std::condition_variable`, `std::atomic<bool>` / `std::atomic<long long>`
- `std::unique_ptr` (only for owning linked-list nodes)
- `std::function<void()>` (only in the thread pool's task queue)
- `std::chrono::steady_clock` for time
- **POSIX socket** functions for networking: `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `close`, `shutdown`, `setsockopt`, `inet_pton`, `htons`, plus `errno` / `strerror`
- `std::cout` / `std::cerr` for output

**Do NOT use:**
- templates (no `template<...>` anywhere), inheritance hierarchies, virtual functions, operator overloading
- `std::optional`, `std::variant`, `std::any`, `std::shared_mutex`, `std::scoped_lock`, `std::jthread`, `std::pmr`, `std::shared_ptr`
- `memory_order_*` arguments, `alignas`, `constexpr` functions, `auto` return types, structured bindings, lambdas longer than 3 lines
- external libraries other than GoogleTest (for tests only)
- `epoll`, `poll`, `select`, or non-blocking sockets (use simple blocking sockets with timeouts)
- macros other than include guards

**Style:**
- Prefer plain loops, plain `if/else`, and small functions with clear names.
- Return results simply, e.g. `bool get(const std::string& key, std::string& out_value)`.
- Each class gets its own `.h` + `.cpp` file.
- Add short comments explaining **why** something is done (for example: "// lock needed even for get, because get moves the node to the front").
- Keep each function under about 40 lines where possible.
- Clear variable names (`capacity`, `head`, `tail`, `node_map`), not one-letter names (except loop counters).
- **All OS socket code lives only in `socket_utils.h/.cpp`.** Every other file calls those helper functions, so the rest of the project is plain C++ that doesn't depend on the OS.

---

## 0. Project summary

A distributed in-memory key-value cache:

1. **LRU cache**: hash map plus hand-written doubly linked list, O(1) get, put and evict, protected by a mutex.
2. **TTL expiry**: keys can expire. They are removed when read (lazy) and by a background cleanup thread.
3. **Statistics**: hits, misses, evictions, expirations and hit rate.
4. **TCP server**: each node is a separate process listening on its own port (7001, 7002, 7003), using a simple **text protocol**.
5. **Consistent hashing**: keys are spread across nodes using a hash ring with virtual nodes.
6. **Client**: sends each key to the correct node over TCP.
7. **Replication**: each key is stored on 2 nodes (primary plus 1 replica).
8. **Failure detection**: nodes ping each other; a node that stops answering is marked DEAD, and reads go to the replica.
9. **Benchmark**: measures throughput and latency, and compares consistent hashing with `hash % N`.

Not included (future work, listed in the README): persistence to disk, strong consistency, security, connection pooling, Windows-native build (only `socket_utils.cpp` would change).

---

## Phase 0 — Environment check (the user has already installed the tools; just verify)

1. Verify in the terminal:
   ```bash
   g++ --version      # 11 or newer
   cmake --version    # 3.16 or newer
   git --version
   nc -h              # netcat, used for manual testing (prints help)
   pwd                # must be under /home/..., NOT /mnt/c/...
   ```
   If `pwd` shows `/mnt/c/...`, stop and tell the user. The project must live in the Linux filesystem, because building under `/mnt/c` is very slow and Windows Smart App Control may interfere.
2. If an old `build/` folder exists (e.g. created on Windows), delete it: `rm -rf build`.
3. If an old `CMakeLists.txt` has Windows settings (`ws2_32`, `WIN32_LEAN_AND_MEAN`, `/W4`, `gtest_force_shared_crt`), replace it according to section 2.
4. If Phase 1 files already exist (`lru_cache.h/.cpp`, `test_lru_cache.cpp`), keep them. They are plain C++ and work on Linux. Only check that they follow the style rules.

**Build commands** (also put these in the README):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Executables are created in `build/` (`dcache_node`, `dcache_cli`, `dcache_bench`, `dcache_tests`).

**Thread-safety check build** (ThreadSanitizer):
```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DUSE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

---

## 1. Folder structure

```
distributed-lru-cache/
├── CMakeLists.txt
├── README.md
├── PROJECT_PLAN.md
├── cluster.conf
├── .gitignore                         # build/, build-tsan/, .pids
├── src/
│   ├── lru_cache.h / lru_cache.cpp        # LRU cache + TTL + stats
│   ├── hash_ring.h / hash_ring.cpp        # consistent hashing
│   ├── socket_utils.h / socket_utils.cpp  # ALL socket code lives here
│   ├── thread_pool.h / thread_pool.cpp    # simple thread pool
│   ├── config.h / config.cpp              # reads cluster.conf
│   ├── server.h / server.cpp              # cache node server
│   ├── client.h / client.cpp              # client that routes keys to nodes
│   ├── node_main.cpp                      # starts one node
│   ├── cli_main.cpp                       # command-line client
│   └── bench_main.cpp                     # benchmarks
├── scripts/
│   ├── run_cluster.sh                     # starts 3 nodes in background, logs to logs/
│   └── stop_cluster.sh                    # stops them
└── tests/
    ├── test_lru_cache.cpp
    ├── test_hash_ring.cpp
    ├── test_socket_utils.cpp
    ├── test_thread_pool.cpp
    └── test_cluster.cpp                   # server + client + replication + failure
```

## 2. CMake setup

- CMake ≥ 3.16, `set(CMAKE_CXX_STANDARD 17)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`.
- Flags: `-Wall -Wextra`. The `Release` build type adds `-O2`.
- `find_package(Threads REQUIRED)` and link `Threads::Threads` to every target.
- A library target `dcache_lib` holds all `src/*.cpp` except the `*_main.cpp` files. The executables `dcache_node`, `dcache_cli` and `dcache_bench` link against it.
- Optional ThreadSanitizer, using a simple block:
  ```cmake
  option(USE_TSAN "Build with ThreadSanitizer" OFF)
  if(USE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
  endif()
  ```
- GoogleTest comes in through `FetchContent` (a recent release tag, e.g. v1.15.x). The test executable `dcache_tests` links `gtest_main`, `dcache_lib` and `Threads::Threads`.
- Use `include(GoogleTest)` and `gtest_discover_tests(dcache_tests DISCOVERY_TIMEOUT 30)` so `ctest` lists each test.

---

## 3. Phases

### Phase 1 — LRU cache (`lru_cache.h/.cpp`)

*(Pure C++, no OS code.)*

```cpp
struct Node {
    std::string key;
    std::string value;
    long long expire_at_ms;   // 0 means "never expires"
    long long version;        // used later for replication
    Node* prev;
    Node* next;
};

class LRUCache {
public:
    explicit LRUCache(int capacity);
    bool get(const std::string& key, std::string& out_value);
    void put(const std::string& key, const std::string& value, long long ttl_ms);
    bool remove(const std::string& key);
    int size();
    std::string stats_string();   // "hits=.. misses=.. evictions=.. expired=.. size=.. hit_rate=.."
private:
    void unlink_node(Node* node);
    void add_to_front(Node* node);
    void evict_lru();
    long long now_ms();

    int capacity;
    Node head;   // dummy node: most recently used is head.next
    Node tail;   // dummy node: least recently used is tail.prev
    std::unordered_map<std::string, std::unique_ptr<Node>> node_map;  // map owns the nodes
    std::mutex mtx;
    long long hits = 0, misses = 0, evictions = 0, expired = 0;
};
```
- `head` and `tail` are **dummy (sentinel) nodes**, so inserting and removing never needs null checks.
- `get` and `put` both lock `mtx` with `std::lock_guard`. Comment why: `get` changes the list order.
- `get`: if the key is found but expired, remove it, count it as expired and a miss, and return false. Otherwise move it to the front and count a hit.
- `put`: if the key exists, update the value and TTL and move it to the front. If it is new and the cache is full, call `evict_lru()` (remove `tail.prev`) first.
- `now_ms()` uses `std::chrono::steady_clock`. Comment why it isn't the system clock: the system clock can jump.
- Add a `bool check_list_ok()` helper for tests: walking forward from `head` visits exactly `node_map.size()` nodes.

**Tests (`test_lru_cache.cpp`):**
- put and get, then overwriting a key
- eviction order: capacity 3, put a, b, c, get a, put d → b is gone
- capacity 1
- removing a key that doesn't exist
- stats counts after a fixed sequence
- TTL: put with `ttl_ms = 100`, sleep 150 ms, get returns false
- multithreaded: 8 threads × 10,000 random put/get calls, then `size() <= capacity` and `check_list_ok()` holds

### Phase 2 — Background expiry thread

*(Pure C++.)*
- Add `start_cleanup_thread(int interval_ms)` and `stop_cleanup_thread()` to `LRUCache`.
- The thread loops while a `std::atomic<bool> running` flag is true. Each loop it sleeps using `condition_variable.wait_for`, so stop can wake it immediately. It then locks the mutex, walks up to 50 nodes from the **tail** end, and removes expired ones.
- The destructor calls `stop_cleanup_thread()`, which sets the flag, notifies the condition variable, and joins the thread.
- Test: put 10 keys with a 100 ms TTL, never read them, wait 300 ms; `size()` is 0.
- **Run the TSan build** (see Phase 0) after this phase. It must show no "data race" warnings.

### Phase 3 — Socket helpers + text protocol (`socket_utils.h/.cpp`)

**This is the ONLY file that includes `<sys/socket.h>`, `<netinet/in.h>`, `<netinet/tcp.h>`, `<arpa/inet.h>`, `<unistd.h>`.**

**Text protocol** (one request per line, one response per line):
```
Request                              Response
PING                                 PONG <node_id>
GET <key>                            VALUE <value>   |  NOT_FOUND
PUT <key> <ttl_ms> <value...>        OK              (ttl_ms = 0 means no expiry; value is rest of line)
DEL <key>                            OK | NOT_FOUND
STATS                                STATS <stats string>
NODES                                NODES n1=ALIVE n2=DEAD n3=ALIVE
LOCAL_GET <key>                      VALUE <value> | NOT_FOUND     (debug: no routing, local only)
REPL_PUT <key> <version> <ttl_ms> <value...>   OK      (node-to-node only)
REPL_DEL <key> <version>                       OK      (node-to-node only)
```
- Keys cannot contain spaces. Values cannot contain newlines. Maximum line length is 64 KB; longer lines get `ERROR line too long` and the connection is closed.
- Lines end with `\n`. When reading, strip a trailing `\r` too, in case a client sends `\r\n`.
- Because it is a text protocol, it can be tested by hand with `nc localhost 7001`.

**Helper functions (the rest of the project uses only these):**
```cpp
int  create_server_socket(int port);        // socket + SO_REUSEADDR + bind 127.0.0.1 + listen; returns fd or -1
int  accept_client(int server_fd);          // returns client fd, or -1 on error/shutdown
int  connect_to(const std::string& host, int port);  // returns fd or -1
void set_timeout(int fd, int ms);           // SO_RCVTIMEO + SO_SNDTIMEO using struct timeval
void set_no_delay(int fd);                  // TCP_NODELAY
bool send_all(int fd, const std::string& data);
void close_socket(int fd);                  // close()
void shutdown_socket(int fd);               // shutdown(fd, SHUT_RDWR) - used to wake a blocked accept()
class LineReader { public: explicit LineReader(int fd); bool read_line(std::string& line); ... };
std::vector<std::string> split(const std::string& line, int max_parts);
```
Details that must be done correctly (add a comment at each one):
- `send_all` loops until every byte is sent, because `send` may send only part of the data. It passes the **`MSG_NOSIGNAL`** flag so that writing to a closed socket returns an error instead of killing the program with SIGPIPE. It retries if `errno == EINTR`.
- `LineReader::read_line` calls `recv` into a buffer until the buffer contains `'\n'`, then returns one line. Comment why: TCP is a stream of bytes, so one `recv` can return half a line or two lines. `recv` returning 0 means the other side closed the connection; `-1` with `EAGAIN`/`EWOULDBLOCK` means a timeout. It retries on `EINTR`.
- `SO_REUSEADDR` on the server socket, so a restarted node can bind the same port immediately (otherwise it may fail with "Address already in use" for about 60 seconds).
- `TCP_NODELAY` on connections, so small request/response messages aren't delayed by Nagle's algorithm.
- `split` splits on spaces; the last part keeps the rest of the line, so values may contain spaces.

**Tests (`test_socket_utils.cpp`):** start a listening socket in a thread, send `"PI"` and then `"NG\n"` in two separate `send` calls, and check that `read_line` returns `"PING"`. Also check that `"PING\r\n"` is read as `"PING"`, and that `split("PUT k 0 hello world", 4)` gives `["PUT","k","0","hello world"]`.

### Phase 4 — Consistent hash ring (`hash_ring.h/.cpp`)

*(Pure C++.)*
```cpp
class HashRing {
public:
    explicit HashRing(int virtual_nodes);
    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);
    std::string get_node(const std::string& key);
    std::vector<std::string> get_nodes(const std::string& key, int count); // distinct physical nodes
private:
    unsigned long long hash_string(const std::string& s);   // FNV-1a 64-bit
    std::map<unsigned long long, std::string> ring;         // position -> node id
    int virtual_nodes;
    std::mutex mtx;
};
```
- `hash_string` uses **FNV-1a** (a few lines). Comment why it isn't `std::hash`: `std::hash` can give different results with different compilers, and every node and client must agree on where a key lives.
- `add_node` inserts `virtual_nodes` positions: `hash(node_id + "#" + std::to_string(i))`.
- `get_node`: `ring.lower_bound(hash(key))`; if that reaches the end, wrap around to `ring.begin()`.
- `get_nodes(key, 2)`: walk clockwise from the key's position and collect node ids, **skipping a node already collected**, until `count` distinct nodes are found. Stop after one full lap.

**Tests (`test_hash_ring.cpp`):**
- the same key always maps to the same node
- 3 nodes, 100 virtual nodes, 30,000 keys → each node gets between 7,000 and 13,000
- 4 nodes → add a 5th: fewer than 30% of keys move, and every moved key moves to the new node
- `get_nodes(key, 2)` returns 2 different nodes
- remove a node → its keys go to other nodes; other keys don't move

### Phase 5 — Thread pool + config + server

**Thread pool (`thread_pool.h/.cpp`)** *(pure C++)*:
- A fixed number of worker threads and a `std::queue<std::function<void()>>` of tasks, protected by a mutex and a condition variable.
- `void submit(std::function<void()> task)` and `void shutdown()` (stop, wake all workers, join them).
- Workers wait with `cv.wait(lock, [&]{ return stopping || !tasks.empty(); })`. Comment why it uses a condition: to handle spurious wake-ups.
- Test: submit 1,000 tasks that each increment an `std::atomic<int>`; after shutdown it equals 1,000.

**Config (`config.h/.cpp`)** reads `cluster.conf`:
```
replication 2
virtual_nodes 100
node n1 127.0.0.1 7001
node n2 127.0.0.1 7002
node n3 127.0.0.1 7003
```
into a simple struct holding a `std::vector<NodeInfo>` (where `NodeInfo` has `id`, `host` and `port`).

**Server (`server.h/.cpp`):**
- `CacheServer(my_id, config, cache_capacity)` with `start()` and `stop()`.
- An accept thread loops on `accept_client()`. Each new client connection is handled by `pool.submit(...)`.
- The connection handler sets a timeout (e.g. 30 s idle) and loops: `read_line` → parse the command → run it → `send_all` the response → repeat until the client disconnects or times out. Then it calls `close_socket`.
- `PUT` on the node that coordinates the write:
  1. `version = current time in ms`
  2. store it locally
  3. queue `REPL_PUT` for the other replica node (Phase 7)
  4. reply `OK`
- `stop()`: set `std::atomic<bool> running = false`, then call `shutdown_socket(listen_fd)` followed by `close_socket(listen_fd)`. Comment why: on Linux, `shutdown()` is what reliably wakes a thread blocked in `accept()`. Then join the accept thread and shut down the pool.
- `node_main.cpp`:
  - `signal(SIGPIPE, SIG_IGN)`. Comment why: extra safety against the process being killed when writing to a closed socket.
  - `signal(SIGINT, handler)` and `signal(SIGTERM, handler)`. The handler only sets a global `std::atomic<bool> stop_requested`; the main thread checks it every 200 ms, then calls `server.stop()`.
  - Command line: `./dcache_node --id n1 --config cluster.conf [--capacity 10000]`. The port comes from the config.
- Log simple lines such as `[n1] PUT user:1 -> stored` using a mutex-protected log function.

**Test (in `test_cluster.cpp`)**: start one server on port 7101 in the test, connect with `connect_to`, send `PUT a 0 hello\n`, then `GET a\n` → `VALUE hello`.

### Phase 6 — Client (`client.h/.cpp`) + CLI

- `CacheClient(config)` builds the same `HashRing` as the servers.
- `bool put(key, value, ttl_ms)`: find the primary with `ring.get_nodes(key, 2)[0]`, connect, send `PUT`, read the reply.
- `bool get(key, std::string& out_value, std::string& served_by)`: try the primary first. If connecting fails or times out, **try the replica** (failover).
- Keeping it simple is fine: open a new connection per request, with a 500 ms timeout. (A connection pool is listed as future work.)
- `cli_main.cpp`:
  ```
  ./dcache_cli put <key> <value> [ttl_ms]
  ./dcache_cli get <key>          → prints value and which node served it
  ./dcache_cli del <key>
  ./dcache_cli stats <node_id>
  ./dcache_cli nodes              → asks any alive node for NODES
  ```

### Phase 7 — Replication

*(Logic is pure C++; it uses the socket helpers.)*
- The coordinating node finds the replica with `ring.get_nodes(key, 2)` and picks the node that isn't itself.
- **Asynchronous replication:** after storing locally, push `"REPL_PUT ..."` onto a `std::queue<std::string>` for that replica. One background thread per peer pops messages and sends them. If the peer is down, drop the message and increment a `replication_failed` counter (it's a cache, so the database still has the data).
- Limit each queue to 10,000 messages; when it is full, drop the message.
- On receiving `REPL_PUT`: store the value **only if the incoming version is greater than the stored version** (add a `put_with_version` method to `LRUCache`). Comment why: messages can arrive out of order.

**Test**: start 3 servers (ports 7201–7203), put 50 keys through the client, wait 200 ms, then send `LOCAL_GET <key>` to each server and check that each key is on exactly 2 servers. Stop the primary of one key; `client.get` still returns the value.

### Phase 8 — Failure detection

- Each server runs a heartbeat thread that sends `PING` to every other node every 1 second, with a 300 ms timeout.
- It keeps `std::map<std::string, int> missed_pings` and `std::map<std::string, bool> alive`, protected by a mutex.
- After **3 missed pings in a row**, the peer is marked DEAD and the server logs `[n1] n2 is DEAD`. One successful ping marks it ALIVE again and logs `[n1] n2 is back ALIVE`.
- `NODES` returns this table.
- The client skips nodes that the `NODES` reply reports as DEAD. It refreshes that list when a request fails.

**Test**: start 3 servers, stop one, wait 4 seconds; the other two report it DEAD. Restart it; within 2 seconds it is ALIVE again.

**After Phases 5–8, run the TSan build.** It must show no "data race" warnings.

### Phase 9 — Benchmark (`bench_main.cpp`)

Keep it simple and print plain tables:
1. **`./dcache_bench local`**: `LRUCache` with 1, 2, 4 and 8 threads doing 1,000,000 random operations (80% get, 20% put). Print ops/second.
2. **`./dcache_bench cluster`**: against the running 3-node cluster, 4 client threads each doing 5,000 requests. Record each request's time in a `std::vector<long long>` (microseconds), sort it, and print average, p50, p95 and p99 latency, plus requests/second.
3. **`./dcache_bench distribution`**:
   - keys per node with 1, 10, 50 and 100 virtual nodes
   - **% of keys that move when going from 4 to 5 nodes: consistent hashing vs `hash % N`** (expected: about 20% vs about 80%)
- Build benchmarks in **Release** mode, never with TSan.
- Note in the README that opening a new connection per request costs time, and that a connection pool would be the next optimisation.

### Phase 10 — Scripts + README

- `scripts/run_cluster.sh`: starts n1, n2 and n3 in the background (`./build/dcache_node --id n1 --config cluster.conf > logs/n1.log 2>&1 &`), saves their PIDs to `.pids`, and prints "cluster started".
- `scripts/stop_cluster.sh`: kills the PIDs in `.pids` and deletes the file.
- Make both executable (`chmod +x scripts/*.sh`).
- README contents:
  - what the project is and why it's useful
  - a simple ASCII architecture diagram
  - how to build, test and run
  - the demo steps (including `nc localhost 7001`)
  - the benchmark results table
  - design decisions:
    - why a hash map plus a doubly linked list
    - why consistent hashing
    - why asynchronous replication
    - why the system chooses availability over consistency (AP)
    - why all socket code is isolated in `socket_utils.cpp`
  - known limitations and future work: connection pooling, sharded locks, gossip protocol, epoll, persistence

---

## 4. Pitfalls to handle (add a code comment where each one is handled)

| # | Pitfall | Fix |
|---|---|---|
| 1 | Linked-list pointer mistakes | Dummy head/tail nodes + `check_list_ok()` test |
| 2 | Thinking `get` doesn't need a lock | `get` moves the node, so lock it |
| 3 | `recv` returns half a line or two lines | `LineReader` with a buffer |
| 4 | `send` sends only part of the data | `send_all` loop |
| 5 | Writing to a closed socket kills the program (SIGPIPE) | `MSG_NOSIGNAL` + `signal(SIGPIPE, SIG_IGN)` |
| 6 | `EINTR` interrupts `send`/`recv` | Retry the call |
| 7 | A dead node blocks a client forever | Socket timeouts |
| 8 | Restarted node can't bind: "Address already in use" | `SO_REUSEADDR` |
| 9 | Blocked `accept()` stops shutdown | `shutdown()` + `close()` the listening socket in `stop()` |
| 10 | Small messages delayed (Nagle's algorithm) | `TCP_NODELAY` |
| 11 | `std::hash` differs between compilers | FNV-1a |
| 12 | Uneven key spread | 100 virtual nodes per server |
| 13 | Both copies of a key on the same server | `get_nodes` skips duplicate physical nodes |
| 14 | An old replicated value overwrites a newer one | Version numbers |
| 15 | System clock jumps break TTL | `steady_clock` |
| 16 | Thread not joined → program crashes on exit | Always `join()` in `stop()` / the destructor |
| 17 | Hidden data races | Run the TSan build after Phases 2, 5, 6, 7, 8 |
| 18 | Replication queue grows without limit | Cap of 10,000 messages |
| 19 | Benchmarking a Debug or TSan build | Always benchmark the Release build |
| 20 | Project under `/mnt/c` → very slow builds | Keep it in `~/projects/` |

---

## 5. Rules for Claude Code

- Follow the **CODING STYLE RULES** at the top. If something seems to need an advanced feature, choose the simpler way and add a comment.
- Run all commands in the **WSL Ubuntu terminal** (bash), not PowerShell.
- Work one phase at a time. After each phase, build, run `ctest`, show the output, and stop for review.
- Don't add features that aren't in this plan.
- Every thread that is started must be stopped and joined.
- Every socket that is opened must be closed.