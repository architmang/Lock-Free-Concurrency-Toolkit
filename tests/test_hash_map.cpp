#include "../include/concurrent_hash_map.hpp"
#include <thread>
#include <vector>
#include <cassert>
#include <cstdio>

using namespace hft;

int main() {
    ConcurrentHashMap<int, int> map(16);

    // basic single-threaded correctness
    assert(!map.find(1).has_value());
    map.insertOrAssign(1, 100);
    assert(map.find(1).value() == 100);
    map.insertOrAssign(1, 200);
    assert(map.find(1).value() == 200);
    assert(map.erase(1));
    assert(!map.find(1).has_value());

    // concurrent inserts of disjoint keys from many threads must all land
    constexpr int numThreads = 8;
    constexpr int keysPerThread = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]{
            for (int i = 0; i < keysPerThread; ++i) {
                int key = t * keysPerThread + i;
                map.insertOrAssign(key, key * 2);
            }
        });
    }
    for (auto& th : threads) th.join();

    assert(map.size() == (size_t)(numThreads * keysPerThread));
    for (int t = 0; t < numThreads; ++t) {
        for (int i = 0; i < keysPerThread; ++i) {
            int key = t * keysPerThread + i;
            auto v = map.find(key);
            assert(v.has_value() && v.value() == key * 2);
        }
    }

    std::printf("test_hash_map: PASSED (%zu concurrent inserts verified)\n", map.size());
    return 0;
}
