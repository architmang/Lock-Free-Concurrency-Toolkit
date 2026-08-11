#include "../include/mpmc_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <cstdio>

using namespace hft;

int main() {
    constexpr size_t Capacity = 1 << 14;
    constexpr int producers = 4;
    constexpr int consumers = 4;
    constexpr size_t opsPerProducer = 200000;
    constexpr size_t totalOps = producers * opsPerProducer;

    MPMCQueue<uint64_t, Capacity> q;
    std::atomic<uint64_t> consumedSum{0};
    std::atomic<size_t> consumedCount{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p]{
            for (size_t i = 0; i < opsPerProducer; ++i) {
                uint64_t value = (uint64_t)p * opsPerProducer + i;
                while (!q.push(value)) { std::this_thread::yield(); }
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&]{
            uint64_t v;
            while (consumedCount.load(std::memory_order_relaxed) < totalOps) {
                if (q.pop(v)) {
                    consumedSum.fetch_add(v, std::memory_order_relaxed);
                    consumedCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    uint64_t expectedSum = 0;
    for (uint64_t i = 0; i < totalOps; ++i) expectedSum += i;

    assert(consumedCount.load() == totalOps);
    assert(consumedSum.load() == expectedSum); // every value consumed exactly once
    std::printf("test_mpmc_queue: PASSED (%zu ops across %d producers / %d consumers)\n",
                totalOps, producers, consumers);
    return 0;
}
