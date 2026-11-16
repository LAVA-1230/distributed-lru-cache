# Distributed LRU Cache (C++17, Linux)

An in-memory key-value cache spread over several processes, like a small Memcached/Redis
cluster. Each node is an LRU cache with TTL expiry. Keys are spread with consistent hashing,
every key is stored on 2 nodes, and nodes detect each other's failures with heartbeats, so
reads keep working when a node dies.

Caches like this sit in front of a database: hot data is served from memory in microseconds
instead of hitting the database on every request.

## Architecture

```
                    dcache_cli / dcache_bench / nc
                               |
                CacheClient: same hash ring as the nodes
                  -> sends key to its primary node,
                     falls back to the replica if it fails
                               |
          +--------------------+--------------------+
          |                    |                    |
    +-----v------+       +-----v------+       +-----v------+
    | node n1    |       | node n2    |       | node n3    |
    | :7001      |<----->| :7002      |<----->| :7003      |
    |            | PING  |            | PING  |            |
    | LRUCache   | REPL_ | LRUCache   | REPL_ | LRUCache   |
    | HashRing   | PUT   | HashRing   | PUT   | HashRing   |
    | ThreadPool |       | ThreadPool |       | ThreadPool |
    +------------+       +------------+       +------------+

  Per node threads: accept thread, worker pool (16), TTL cleanup thread,
                    heartbeat thread, one replication sender per peer.
```

| File | What it does |
|---|---|
| `src/lru_cache.*` | Hash map + doubly linked list, O(1) get/put/evict, TTL, stats, versions |
| `src/hash_ring.*` | Consistent hashing with 100 virtual nodes per server (FNV-1a + mixing) |
| `src/socket_utils.*` | **All** POSIX socket code: connect, accept, `send_all`, `LineReader`, timeouts |
| `src/thread_pool.*` | Fixed worker threads + task queue |
| `src/config.*` | Reads `cluster.conf` |
| `src/server.*` | Text protocol, routing, async replication, heartbeats |
| `src/client.*` | Routes keys to nodes, fails over to the replica |

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Executables are created in `build/` (`dcache_node`, `dcache_cli`, `dcache_bench`, `dcache_tests`).

ThreadSanitizer build (checks for data races):
```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DUSE_TSAN=ON
setarch $(uname -m) -R cmake --build build-tsan -j
setarch $(uname -m) -R ctest --test-dir build-tsan --output-on-failure
```
`setarch -R` turns off address randomization for that command only. Recent WSL kernels
randomize addresses more than GCC 13's TSan supports, so without it every TSan binary
fails with `FATAL: ThreadSanitizer: unexpected memory mapping`.

## Demo

```bash
./scripts/run_cluster.sh                 # starts n1, n2, n3; logs in logs/

./build/dcache_cli put user:1 alice
./build/dcache_cli put session:9 temp 3000     # expires after 3 s
./build/dcache_cli get user:1                  # alice   (served by n1)
./build/dcache_cli nodes                       # NODES n1=ALIVE n2=ALIVE n3=ALIVE
./build/dcache_cli stats n1

nc localhost 7001                              # talk the protocol by hand
PING
PUT greeting 0 hello world
GET greeting
NODES

kill <pid of n1>                               # first line of .pids
./build/dcache_cli get user:1                  # alice   (served by n2)  <- failover
sleep 4; ./build/dcache_cli nodes              # n1=DEAD
tail logs/n2.log                               # [n2] n1 is DEAD

./scripts/stop_cluster.sh
```

### Protocol

```
PING                               -> PONG <node_id>
GET <key>                          -> VALUE <value> | NOT_FOUND
PUT <key> <ttl_ms> <value...>      -> OK                (ttl_ms 0 = never expires)
DEL <key>                          -> OK | NOT_FOUND
STATS                              -> STATS hits=.. misses=.. evictions=.. expired=.. size=.. hit_rate=.. replication_failed=..
NODES                              -> NODES n1=ALIVE n2=DEAD n3=ALIVE
LOCAL_GET <key>                    -> VALUE <value> | NOT_FOUND   (this node only, no routing)
REPL_PUT <key> <ver> <ttl> <value> -> OK                (node to node)
REPL_DEL <key> <ver>               -> OK                (node to node)
```
A node that doesn't own a key forwards GET/PUT/DEL to an owner, so any node can be asked.

## Benchmark results

Release build, WSL2 Ubuntu 24.04, g++ 13.3, 3 nodes on one machine.

`./build/dcache_bench local` (1,000,000 ops, 80% get / 20% put):

| threads | ops/sec |
|---|---|
| 1 | 8,576,844 |
| 2 | 1,865,487 |
| 4 | 1,696,028 |
| 8 | 1,760,105 |

More threads are *slower*: every operation takes the same single mutex, so threads spend
their time waiting for each other (lock contention). Sharded locks are the fix (see below).

`./build/dcache_bench cluster` (4 threads x 5,000 requests, new TCP connection per request):

| metric | value |
|---|---|
| requests/sec | 24,790 |
| avg | 157 us |
| p50 | 143 us |
| p95 | 269 us |
| p99 | 458 us |

Most of that time is opening a new TCP connection for every request. A connection pool
would be the next optimisation.

`./build/dcache_bench distribution` (30,000 keys):

| virtual nodes | n1 | n2 | n3 | max/min |
|---|---|---|---|---|
| 1 | 18,781 | 1,322 | 9,897 | 14.21 |
| 10 | 7,691 | 10,093 | 12,216 | 1.59 |
| 50 | 8,995 | 12,393 | 8,612 | 1.44 |
| 100 | 8,478 | 11,529 | 9,993 | 1.36 |

Keys that move when going from 4 to 5 nodes: **consistent hashing 20.0 %**, **hash % N 80.3 %**.

## Design decisions

- **Hash map + doubly linked list.** The map finds a node in O(1); the list keeps use order,
  so moving a node to the front and evicting from the back are O(1) pointer changes. Dummy
  head/tail nodes remove all null checks. The map owns nodes through `unique_ptr`.
- **Consistent hashing.** With `hash % N`, adding a node moves ~80% of keys (a cache miss
  storm). On a ring, only the keys in the new node's slices move (~1/N). 100 virtual nodes
  per server keep the spread even. FNV-1a is used instead of `std::hash` because every
  process must compute the same hash; a final mixing step fixes FNV-1a's weak high bits for
  keys that differ only at the end.
- **Asynchronous replication.** The coordinator replies OK after storing locally and queues
  the copy for the replica. Writes stay fast and don't fail when the replica is down.
  Version numbers (time-based, strictly increasing) stop an older copy overwriting a newer one.
- **Availability over consistency (AP).** When a node dies, reads go to the replica, which
  may briefly have an older value. For a cache this is the right trade: stale-for-a-moment
  is better than unavailable, and the database remains the source of truth.
- **All socket code in `socket_utils.cpp`.** The rest of the project is plain portable C++.
  Pitfalls are handled in one place: partial `send`, `recv` returning half or two lines,
  `EINTR`, SIGPIPE (`MSG_NOSIGNAL`), timeouts, `SO_REUSEADDR`, `TCP_NODELAY`, waking a
  blocked `accept()` with `shutdown()`.

## Known limitations and future work

- **Connection pooling.** The client opens a new TCP connection per request.
- **Sharded locks.** One mutex per cache limits multi-core scaling (see the local benchmark).
- **Gossip protocol.** Every node pings every other node: O(N²) messages.
- **epoll.** One blocked worker thread per open connection limits the number of clients.
- **Persistence.** Everything is lost when a node restarts.
- **No catch-up after a restart.** A restarted node starts empty and doesn't copy the keys
  it missed; writes made while it was down only exist on the replica.
- **Clock-based versions.** Fine on one machine; across machines, clock skew could make a
  newer write lose. No security or authentication. Linux only (only `socket_utils.cpp`
  would change for Windows).
