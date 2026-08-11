#pragma once
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <optional>

namespace hft {

// Concurrent hash map optimized for read-heavy workloads (~99% reads),
// e.g. a symbol -> instrument-config or symbol -> last-trade-price table
// that's updated occasionally but read on every single order.
//
// Design: the key space is split into N independent shards, each
// guarded by its own std::shared_mutex. Reads take a *shared* lock, so
// any number of readers can be inside the same shard at once with zero
// contention between them; a write takes the *exclusive* lock, and it
// only blocks the ONE shard being written, not the whole map. With
// enough shards, write contention across different keys becomes rare,
// and read throughput scales close to linearly with reader count.
//
// This is intentionally not a wait-free/lock-free hash map -- a true
// lock-free hash map needs hazard pointers or epoch-based reclamation
// to make it safe to free a bucket's old memory while a reader might
// still be dereferencing it, which is a meaningfully bigger undertaking.
// The sharded shared_mutex design gets you most of the real-world
// benefit (readers never block other readers, writers only block their
// own shard) for a fraction of the complexity and risk. See the README
// for how you'd extend this to a seqlock-per-bucket or hazard-pointer
// design if you need writers to never block readers at all.
template <typename K, typename V, typename Hash = std::hash<K>>
class ConcurrentHashMap {
public:
    explicit ConcurrentHashMap(size_t shardCount = 64) : shards_(shardCount) {}

    void insertOrAssign(const K& key, const V& value) {
        Shard& s = shardFor(key);
        std::unique_lock lock(s.mutex);
        s.map[key] = value;
    }

    bool erase(const K& key) {
        Shard& s = shardFor(key);
        std::unique_lock lock(s.mutex);
        return s.map.erase(key) > 0;
    }

    std::optional<V> find(const K& key) const {
        const Shard& s = shardFor(key);
        std::shared_lock lock(s.mutex); // concurrent readers proceed without blocking each other
        auto it = s.map.find(key);
        if (it == s.map.end()) return std::nullopt;
        return it->second;
    }

    size_t size() const {
        size_t total = 0;
        for (auto& s : shards_) {
            std::shared_lock lock(s.mutex);
            total += s.map.size();
        }
        return total;
    }

private:
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<K, V, Hash> map;
    };

    Shard& shardFor(const K& key) { return shards_[Hash{}(key) % shards_.size()]; }
    const Shard& shardFor(const K& key) const { return shards_[Hash{}(key) % shards_.size()]; }

    std::vector<Shard> shards_;
};

// Naive baseline: a single global mutex guarding one unordered_map.
// Every reader blocks every other reader here -- this is exactly the
// pattern the sharded/shared_mutex design above is built to beat.
template <typename K, typename V, typename Hash = std::hash<K>>
class GlobalMutexHashMap {
public:
    void insertOrAssign(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_[key] = value;
    }
    std::optional<V> find(const K& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
private:
    mutable std::mutex mutex_;
    std::unordered_map<K, V, Hash> map_;
};

} // namespace hft
