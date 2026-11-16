#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "client.h"
#include "config.h"
#include "hash_ring.h"
#include "lru_cache.h"

// Always run benchmarks on the Release build, never Debug or TSan:
// those builds are many times slower and the numbers would mean nothing.

static long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- local: LRUCache only, no network ----------------

static void local_worker(LRUCache* cache, const std::vector<std::string>* keys, int ops, int seed) {
    std::mt19937 rng(seed);
    std::string value;
    for (int i = 0; i < ops; i++) {
        const std::string& key = (*keys)[rng() % keys->size()];
        if (rng() % 100 < 80) {
            cache->get(key, value);       // 80% reads
        } else {
            cache->put(key, "value", 0);  // 20% writes
        }
    }
}

static void bench_local() {
    const int total_ops = 1000000;
    std::vector<std::string> keys;
    for (int i = 0; i < 100000; i++) {
        keys.push_back("key:" + std::to_string(i));
    }

    std::cout << "LRUCache, " << total_ops << " ops (80% get / 20% put), capacity 50000\n";
    std::cout << "threads    ops/sec\n";
    int thread_counts[] = {1, 2, 4, 8};
    for (int t = 0; t < 4; t++) {
        int threads = thread_counts[t];
        LRUCache cache(50000);
        std::vector<std::thread> workers;

        long long start = now_us();
        for (int i = 0; i < threads; i++) {
            workers.push_back(std::thread(local_worker, &cache, &keys, total_ops / threads, i));
        }
        for (size_t i = 0; i < workers.size(); i++) {
            workers[i].join();
        }
        double seconds = (now_us() - start) / 1000000.0;
        std::cout << std::setw(7) << threads << "    " << std::fixed << std::setprecision(0)
                  << total_ops / seconds << "\n";
    }
}

// ---------------- cluster: real requests over TCP ----------------

static void cluster_worker(const ClusterConfig* config, int requests, int seed,
                           std::vector<long long>* latencies, int* errors) {
    CacheClient client(*config);
    std::mt19937 rng(seed);
    std::string value;
    std::string served_by;
    for (int i = 0; i < requests; i++) {
        std::string key = "bench:" + std::to_string(rng() % 1000);
        long long start = now_us();
        bool ok;
        if (rng() % 100 < 80) {
            ok = client.get(key, value, served_by);
        } else {
            ok = client.put(key, "value-" + std::to_string(i), 0);
        }
        latencies->push_back(now_us() - start);
        if (!ok) {
            (*errors)++;
        }
    }
}

// Prints a text bar chart: how many requests fell into each latency range.
static void print_histogram(const std::vector<long long>& sorted) {
    long long limits[] = {100, 200, 300, 500, 1000, 2000, 5000, 10000};
    std::string names[] = {"   <100us", "100-200us", "200-300us", "300-500us",
                           "0.5-1 ms ", "  1-2 ms ", "  2-5 ms ", " 5-10 ms ", "  >10 ms "};
    std::vector<long long> counts(9, 0);
    for (size_t i = 0; i < sorted.size(); i++) {
        int bucket = 8;
        for (int b = 0; b < 8; b++) {
            if (sorted[i] < limits[b]) {
                bucket = b;
                break;
            }
        }
        counts[bucket]++;
    }
    long long biggest = *std::max_element(counts.begin(), counts.end());

    std::cout << "\nLatency histogram (each # = " << std::max(1LL, biggest / 50) << " requests)\n";
    for (int b = 0; b < 9; b++) {
        int bar = (int)(counts[b] * 50 / std::max(1LL, biggest));
        std::cout << names[b] << " | " << std::string(bar, '#') << " " << counts[b] << "\n";
    }
}

static void print_latency_table(std::vector<long long>& latencies, double seconds, int errors) {
    std::sort(latencies.begin(), latencies.end());
    long long sum = 0;
    for (size_t i = 0; i < latencies.size(); i++) {
        sum += latencies[i];
    }
    size_t n = latencies.size();
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "requests     " << n << " (errors: " << errors << ")\n";
    std::cout << "req/sec      " << n / seconds << "\n";
    std::cout << "avg          " << sum / (long long)n << " us\n";
    std::cout << "p50          " << latencies[n * 50 / 100] << " us\n";
    std::cout << "p95          " << latencies[n * 95 / 100] << " us\n";
    std::cout << "p99          " << latencies[n * 99 / 100] << " us\n";
    std::cout << "max          " << latencies[n - 1] << " us\n";
    print_histogram(latencies);
}

static int bench_cluster(const ClusterConfig& config) {
    const int threads = 4;
    const int requests_per_thread = 5000;

    // Fill some keys first so most GETs are hits.
    CacheClient loader(config);
    for (int i = 0; i < 1000; i++) {
        if (!loader.put("bench:" + std::to_string(i), "value", 0)) {
            std::cerr << "cannot reach the cluster - start it with scripts/run_cluster.sh\n";
            return 1;
        }
    }

    std::vector<std::vector<long long>> per_thread(threads);
    std::vector<int> errors(threads, 0);
    std::vector<std::thread> workers;
    long long start = now_us();
    for (int i = 0; i < threads; i++) {
        workers.push_back(std::thread(cluster_worker, &config, requests_per_thread, i,
                                      &per_thread[i], &errors[i]));
    }
    for (size_t i = 0; i < workers.size(); i++) {
        workers[i].join();
    }
    double seconds = (now_us() - start) / 1000000.0;

    std::vector<long long> all;
    int total_errors = 0;
    for (int i = 0; i < threads; i++) {
        all.insert(all.end(), per_thread[i].begin(), per_thread[i].end());
        total_errors += errors[i];
    }
    std::cout << threads << " client threads x " << requests_per_thread
              << " requests (80% get / 20% put), new TCP connection per request\n";
    print_latency_table(all, seconds, total_errors);
    return 0;
}

// ---------------- distribution: how evenly keys spread ----------------

static void bench_spread() {
    const int key_count = 30000;
    std::cout << "Keys per node, 3 nodes, " << key_count << " keys\n";
    std::cout << "vnodes       n1       n2       n3    max/min\n";
    int vnode_options[] = {1, 10, 50, 100};
    for (int v = 0; v < 4; v++) {
        HashRing ring(vnode_options[v]);
        ring.add_node("n1");
        ring.add_node("n2");
        ring.add_node("n3");
        int counts[3] = {0, 0, 0};
        for (int i = 0; i < key_count; i++) {
            std::string node = ring.get_node("key:" + std::to_string(i));
            counts[node[1] - '1']++;   // "n1" -> 0, "n2" -> 1, "n3" -> 2
        }
        int max_count = std::max(counts[0], std::max(counts[1], counts[2]));
        int min_count = std::max(1, std::min(counts[0], std::min(counts[1], counts[2])));
        std::cout << std::setw(6) << vnode_options[v] << std::setw(9) << counts[0]
                  << std::setw(9) << counts[1] << std::setw(9) << counts[2]
                  << std::setw(11) << std::setprecision(2) << std::fixed
                  << (double)max_count / min_count << "\n";
    }
}

static void bench_movement() {
    const int key_count = 30000;
    HashRing ring4(100);
    HashRing ring5(100);
    for (int i = 1; i <= 5; i++) {
        std::string id = "n" + std::to_string(i);
        if (i <= 4) {
            ring4.add_node(id);
        }
        ring5.add_node(id);
    }

    int moved_ring = 0;
    int moved_mod = 0;
    for (int i = 0; i < key_count; i++) {
        std::string key = "key:" + std::to_string(i);
        if (ring4.get_node(key) != ring5.get_node(key)) {
            moved_ring++;
        }
        unsigned long long h = fnv1a_hash(key);
        if (h % 4 != h % 5) {
            moved_mod++;
        }
    }
    std::cout << "\nKeys that move when going from 4 to 5 nodes (" << key_count << " keys)\n";
    std::cout << std::setprecision(1) << std::fixed;
    std::cout << "consistent hashing   " << 100.0 * moved_ring / key_count << " %\n";
    std::cout << "hash % N             " << 100.0 * moved_mod / key_count << " %\n";
}

int main(int argc, char* argv[]) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "local") {
        bench_local();
        return 0;
    }
    if (mode == "distribution") {
        bench_spread();
        bench_movement();
        return 0;
    }
    if (mode == "cluster") {
        ClusterConfig config;
        std::string error;
        std::string path = argc > 2 ? argv[2] : "cluster.conf";
        if (!load_config(path, config, error)) {
            std::cerr << "config error: " << error << std::endl;
            return 1;
        }
        return bench_cluster(config);
    }
    std::cerr << "usage: dcache_bench local | cluster [config] | distribution" << std::endl;
    return 1;
}
