// Showpiece benchmark for the concurrent hash map: many threads hammer
// the map with a 99%-read / 1%-write workload (the realistic profile
// for something like a symbol -> last-price lookup table), comparing
// the sharded shared_mutex design against a single global mutex.
#include "../include/concurrent_hash_map.hpp"
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>

using namespace hft;
using Clock = std::chrono::steady_clock;

template <typename Map>
double runBenchmark(const char* name, int numThreads, size_t opsPerThread, double writeRatio) {
    Map map;
    for (int i = 0; i < 10000; ++i) map.insertOrAssign(i, i * 2);

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]{
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> keyDist(0, 9999);
            std::uniform_real_distribution<double> opDist(0.0, 1.0);
            while (!start.load(std::memory_order_acquire)) { /* spin until go */ }
            for (size_t i = 0; i < opsPerThread; ++i) {
                int key = keyDist(rng);
                if (opDist(rng) < writeRatio) {
                    map.insertOrAssign(key, key * 2 + 1);
                } else {
                    auto v = map.find(key);
                    asm volatile("" : : "g"(v) : "memory"); // prevent the read from being optimized away
                }
            }
        });
    }

    auto t0 = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t1 = Clock::now();

    double seconds = std::chrono::duration<double>(t1 - t0).count();
    size_t totalOps = static_cast<size_t>(numThreads) * opsPerThread;
    double throughput = totalOps / seconds;
    std::printf("%-28s threads=%-3d ops=%-10zu time=%7.3fs  throughput=%12.0f ops/sec\n",
                name, numThreads, totalOps, seconds, throughput);
    return throughput;
}

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    size_t opsPerThread = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 500000;
    double writeRatio = argc > 3 ? std::atof(argv[3]) : 0.01; // default: 99% reads

    std::printf("=== Concurrent Hash Map Benchmark (%d threads, %.0f%% reads) ===\n",
               threads, (1 - writeRatio) * 100);
    double sharded = runBenchmark<ConcurrentHashMap<int,int>>(
        "sharded shared_mutex map", threads, opsPerThread, writeRatio);
    double global = runBenchmark<GlobalMutexHashMap<int,int>>(
        "single global-mutex map", threads, opsPerThread, writeRatio);

    std::printf("\nsharded speedup: %.2fx\n", sharded / global);
    return 0;
}
