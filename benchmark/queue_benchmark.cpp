// Showpiece benchmark for the MPMC queue: N producers and N consumers
// hammer the same queue concurrently, and we measure aggregate
// throughput for the lock-free queue vs. a std::mutex + std::deque
// baseline under identical contention.
#include "../include/mpmc_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace hft;
using Clock = std::chrono::steady_clock;

template <typename Queue>
double runBenchmark(const char* name, int producers, int consumers, size_t opsPerProducer) {
    // Heap-allocated rather than a local variable: MPMCQueue stores its
    // slots as a plain in-class array (not a heap-backed buffer), so at
    // larger capacities the object itself is tens of megabytes -- too
    // big to safely put on the stack.
    auto queuePtr = std::make_unique<Queue>();
    Queue& q = *queuePtr;
    std::atomic<bool> start{false};
    std::atomic<size_t> totalPopped{0};
    size_t totalOps = static_cast<size_t>(producers) * opsPerProducer;

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&]{
            while (!start.load(std::memory_order_acquire)) { /* spin until go */ }
            Backoff backoff;
            for (size_t i = 0; i < opsPerProducer; ++i) {
                while (!q.push(static_cast<int>(i))) {
                    backoff.pause(); // queue momentarily full: back off instead of hammering it
                }
                backoff.reset();
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&]{
            while (!start.load(std::memory_order_acquire)) { /* spin until go */ }
            int v;
            Backoff backoff;
            while (totalPopped.load(std::memory_order_relaxed) < totalOps) {
                if (q.pop(v)) {
                    totalPopped.fetch_add(1, std::memory_order_relaxed);
                    backoff.reset();
                } else {
                    backoff.pause(); // queue momentarily empty: back off instead of hammering it
                }
            }
        });
    }

    auto t0 = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    double seconds = std::chrono::duration<double>(t1 - t0).count();
    double throughput = totalOps / seconds;
    std::printf("%-25s producers=%d consumers=%d ops=%-9zu time=%7.3fs  throughput=%12.0f ops/sec\n",
                name, producers, consumers, totalOps, seconds, throughput);
    return throughput;
}

int main(int argc, char** argv) {
    int producers = argc > 1 ? std::atoi(argv[1]) : 4;
    int consumers = argc > 2 ? std::atoi(argv[2]) : 4;
    size_t opsPerProducer = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1'000'000;

    std::printf("=== MPMC Queue Benchmark (%d producers / %d consumers, contended) ===\n",
                producers, consumers);
    double lockfree = runBenchmark<MPMCQueue<int, 1 << 18>>(
        "lock-free MPMCQueue", producers, consumers, opsPerProducer);
    double locked = runBenchmark<MutexQueue<int>>(
        "mutex+deque baseline", producers, consumers, opsPerProducer);

    std::printf("\nlock-free speedup: %.2fx\n", lockfree / locked);
    return 0;
}
