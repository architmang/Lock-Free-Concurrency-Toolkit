#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <deque>
#include "platform.hpp"

namespace hft {

// Exponential backoff using a CPU "spin-wait hint" instruction (PAUSE on
// x86_64, YIELD on ARM64 -- see platform.hpp).
//
// Why this matters under contention: when a CAS fails, it's because
// another thread just wrote the same cache line we're spinning on.
// Retrying immediately means every core involved keeps invalidating
// each other's cache line on every attempt (cache-line ping-pong),
// which burns memory bandwidth and slows *everyone* down, not just
// the loser of the race. The spin-hint instruction tells the core
// "I'm in a spin loop," which reduces speculative memory traffic and
// gives the coherence protocol time to settle before we hammer the
// line again. Backing off exponentially (more hints each retry, capped)
// trades a little latency in the rare high-contention case for much
// less bus traffic.
class Backoff {
public:
    void pause() {
        for (int i = 0; i < spins_; ++i) cpu_relax();
        if (spins_ < kMaxSpins) spins_ <<= 1;
    }
    void reset() { spins_ = kMinSpins; }
private:
    static constexpr int kMinSpins = 4;
    static constexpr int kMaxSpins = 1024;
    int spins_ = kMinSpins;
};

// 64 bytes is the cache line size on essentially every mainstream x86_64
// and ARM64 chip (including Apple Silicon). We hardcode it rather than
// querying std::hardware_destructive_interference_size because that
// feature-test macro is unreliable across standard library
// implementations -- some (e.g. AppleClang's libc++) advertise
// __cpp_lib_hardware_interference_size as defined without actually
// providing the member, which breaks the build.
constexpr size_t MPMC_CACHE_LINE = 64;

// Bounded multi-producer / multi-consumer lock-free queue.
//
// Based on Dmitry Vyukov's MPMC bounded queue design: storage is one
// contiguous array of slots (good cache locality, no pointer chasing
// like a linked-list queue would have), and each slot owns its own
// sequence number that flags whether it currently holds a value ready
// to be popped, or is free for a push. No slot is ever touched by two
// producers or two consumers at the same time, and the whole thing
// needs zero locks and zero unbounded CAS retry loops (a failed CAS
// just means someone else moved the counter, so we reload and retry
// against the *new* state -- it always makes forward progress).
//
// Cache-line padding: `Slot` is `alignas(64)`, so slot i and slot i+1
// never share a cache line. Without this, a producer writing slot i
// and a consumer reading slot i+1 would ping-pong the same cache line
// between cores on every operation (false sharing) even though they
// touch logically unrelated data.
template <typename T, size_t Capacity>
class MPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    struct alignas(MPMC_CACHE_LINE) Slot {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    MPMCQueue() {
        for (size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& item) {
        Slot* slot;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        Backoff backoff;
        for (;;) {
            slot = &slots_[pos & (Capacity - 1)];
            // Prefetch the slot one ahead of this one: if this push
            // succeeds, the very next call will touch it immediately.
            __builtin_prefetch(&slots_[(pos + 1) & (Capacity - 1)], 1 /* write */, 3);
            size_t seq = slot->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
                backoff.pause(); // lost the CAS race: another producer just touched enqueuePos_
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
                backoff.pause();
            }
        }
        slot->data = item;
        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        Slot* slot;
        size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        Backoff backoff;
        for (;;) {
            slot = &slots_[pos & (Capacity - 1)];
            __builtin_prefetch(&slots_[(pos + 1) & (Capacity - 1)], 0 /* read */, 3);
            size_t seq = slot->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
                backoff.pause();
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
                backoff.pause();
            }
        }
        out = slot->data;
        slot->sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }

private:
    alignas(MPMC_CACHE_LINE) std::atomic<size_t> enqueuePos_;
    alignas(MPMC_CACHE_LINE) std::atomic<size_t> dequeuePos_;
    Slot slots_[Capacity];
};

// Naive std::mutex + std::deque queue, used purely as the "traditional
// locked" comparison point in the benchmark. Unbounded (push always
// succeeds) which is a real structural advantage over the bounded
// lock-free queue -- called out explicitly in the README so the
// comparison isn't misleading.
template <typename T>
class MutexQueue {
public:
    bool push(const T& item) {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.push_back(item);
        return true;
    }
    bool pop(T& out) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }
private:
    std::mutex mutex_;
    std::deque<T> queue_;
};

} // namespace hft
