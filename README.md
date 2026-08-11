# Lock-Free, Cache-Optimized Data Structures

A small library of concurrency primitives in C++: a bounded multi-producer/multi-consumer queue and a concurrent hash map tuned for read-heavy workloads. Both come with correctness tests that run under real multi-threading, not just single-threaded sanity checks, and both are benchmarked against a naive locked baseline so the comparison isn't just a claim.

I built this alongside a matching engine project because the two go together. A matching engine needs somewhere fast to push work between threads and somewhere fast to look things up, and I wanted to actually build those primitives myself instead of reaching for a library and taking cache-line padding and CAS loops on faith.

## Platform support

Builds and runs on x86_64 Linux and ARM64 macOS (Apple Silicon). The one piece that's genuinely different per architecture is the "back off and stop hammering this cache line" instruction used while spinning on a failed compare-and-swap: `PAUSE` on x86_64, `YIELD` on ARM64. That's isolated in `include/platform.hpp` behind a single `cpu_relax()` function, so nothing else in the codebase needs to know which chip it's running on.

Cache line size is hardcoded to 64 bytes rather than pulled from `std::hardware_destructive_interference_size`. That standard feature exists specifically for this kind of padding, but at least one common toolchain (AppleClang's libc++) advertises support for it without actually implementing it, which breaks the build. 64 bytes is correct for basically every mainstream x86_64 and ARM64 chip anyway, so hardcoding it sidesteps a compiler bug for no real downside.

Haven't tested on Windows. Everything assumes a POSIX toolchain (Linux, macOS, or WSL).

## How it's structured

```
                    +----------------------------+
   producer thread  |      MPMCQueue<T>          |  consumer thread
   -----push()---->|  contiguous, padded slots  |----pop()----->
                    |  each slot: seq number      |
                    |  + data, alignas(64)         |
                    +----------------------------+
                         compared against
                    +----------------------------+
                    |   MutexQueue<T> (baseline)  |
                    |   std::mutex + std::deque   |
                    +----------------------------+


                    +----------------------------+
   reader thread    |   ConcurrentHashMap<K,V>    |  writer thread
   ----find()----->|   N shards, each guarded    |<---insertOrAssign()
                    |   by its own shared_mutex   |
                    |   readers: shared lock       |
                    |   writers: exclusive lock    |
                    +----------------------------+
                         compared against
                    +----------------------------+
                    | GlobalMutexHashMap (baseline)|
                    |  one mutex for everything     |
                    +----------------------------+
```

### The MPMC queue

This is a Vyukov-style bounded ring buffer. A few design choices worth walking through.

**Contiguous storage.** It's one flat array of slots, not a linked list. That means sequential memory access instead of pointer chasing, which is most of why this kind of queue is fast in the first place.

**Per-slot sequence numbers.** Each slot tracks whether it's currently holding a value ready to be popped or sitting free for a push. That's what lets two producers or two consumers race for the same slot without ever both grabbing it, and it's also what stops a thread from reading a half-written value.

**Padding to avoid false sharing.** Every slot is `alignas(64)`. Without that, adjacent slots would share a cache line, so a producer writing slot `i` and a consumer reading slot `i+1` would keep invalidating each other's cache line even though they're touching completely unrelated data. That's false sharing, and it's a real, measurable slowdown that has nothing to do with your actual algorithm.

**Backoff on contention.** When a compare-and-swap fails, it's because another thread just touched the same cache line. Retrying instantly just means every core involved keeps invalidating that line over and over. The `Backoff` class spins the CPU-relax instruction a few times, doubling the count on each failed attempt up to a cap, which trades a small amount of latency in the rare contended case for a lot less memory bus traffic.

**Prefetching.** Both `push()` and `pop()` issue a prefetch on the next slot they'll touch, since a tight producer/consumer loop almost always needs it immediately after the current one.

### The concurrent hash map

Split into a fixed number of independent shards, each with its own `std::shared_mutex`. Reads take a shared lock, so any number of readers can be inside the same shard at once without blocking each other. Writes take the exclusive lock, but that only blocks the one shard being written, not the whole map. With enough shards, two threads writing different keys almost never collide.

This is not a fully lock-free hash map. A real lock-free hash map needs hazard pointers or epoch-based memory reclamation to make it safe to free an old bucket while a reader might still be looking at it, and that's a meaningfully bigger and riskier thing to build correctly. The sharded, shared-mutex version gets you most of the real benefit (readers never block other readers, writers only ever block their own shard) for a fraction of the complexity, and it's the design most production systems actually reach for. If I wanted to go further, the next step would be a seqlock per bucket, where readers retry against a version counter instead of taking a lock at all.

## Correctness

Both structures are tested under real concurrency, with multiple threads actually racing against each other, and the tests assert an exact result rather than just "it didn't crash."

```
$ g++ -std=c++17 -O2 -pthread tests/test_mpmc_queue.cpp -o test_mpmc_queue && ./test_mpmc_queue
test_mpmc_queue: PASSED (800000 ops across 4 producers / 4 consumers)

$ g++ -std=c++17 -O2 -pthread tests/test_hash_map.cpp -o test_hash_map && ./test_hash_map
test_hash_map: PASSED (40000 concurrent inserts verified)
```

The queue test has four producers and four consumers racing on the same queue, then checks that every value produced was consumed exactly once, no duplicates, nothing lost. The hash map test has eight threads inserting disjoint keys concurrently, then checks every single one landed with the right value.

## Benchmarks and an honest result

```bash
mkdir build && cd build && cmake .. && make
./queue_benchmark <producers> <consumers> <ops_per_producer>
./hashmap_benchmark <threads> <ops_per_thread> <write_ratio>
```

When I first ran these on a constrained single-core environment, the lock-free queue actually lost to the plain mutex baseline, by a wide margin. Once tested on real multi-core hardware (an M-series MacBook), the gap closed a lot but the mutex baseline was still ahead:

```
=== MPMC Queue Benchmark (4 producers / 4 consumers, contended) ===
lock-free MPMCQueue       throughput=  16,511,615 ops/sec
mutex+deque baseline      throughput=  23,873,637 ops/sec
lock-free speedup: 0.69x

=== Concurrent Hash Map Benchmark (8 threads, 99% reads) ===
sharded shared_mutex map   throughput=  15,290,722 ops/sec
single global-mutex map    throughput=  14,495,677 ops/sec
sharded speedup: 1.05x
```

That queue result led me to look harder at the benchmark harness itself, and I found a real bug in it: the producer's retry loop, for when the (bounded) lock-free queue is momentarily full, was a naked `while` loop with no backoff at all. Meanwhile the mutex-backed baseline is unbounded, so its push never fails and never has to retry. That's not an apples-to-apples comparison. I fixed the harness to back off properly on both the full and empty conditions using the same `Backoff` class the queue itself uses internally, and increased the queue's capacity so producers hit the full condition far less often at these throughput levels (the queue's storage is a plain in-class array rather than heap-allocated, so I also had to switch the benchmark to heap-allocate the queue object itself, since the earlier oversized-capacity version quietly overflowed the stack).

With that fix in place, spin-based lock-free code still fundamentally assumes each thread owns its own physical core. When that's true, spinning costs a bit of power and nothing else, since no other thread is waiting on that core anyway. When threads outnumber cores, a spinning thread wastes cycles a mutex would have handed back to the scheduler for someone who could actually make progress. `std::mutex` on macOS is backed by `os_unfair_lock`, which is unusually well optimized, so this specific comparison is a harder one for the lock-free queue to win than it would be against, say, glibc's mutex on Linux.

**Numbers after the harness fix, rerun on real hardware:** [fill in after rerunning `./queue_benchmark 4 4 1000000` and `./hashmap_benchmark 8 500000 0.01` with the updated benchmark code]

## Layout

```
include/mpmc_queue.hpp           Vyukov MPMC queue, Backoff, MutexQueue baseline
include/concurrent_hash_map.hpp  sharded shared_mutex map, GlobalMutexHashMap baseline
include/platform.hpp             cross-platform spin-wait hint (x86_64 / ARM64)
benchmark/queue_benchmark.cpp    contended throughput: lock-free vs mutex+deque
benchmark/hashmap_benchmark.cpp  99%-read throughput: sharded vs global mutex
tests/test_mpmc_queue.cpp        multi-producer/multi-consumer correctness stress test
tests/test_hash_map.cpp          concurrent-insert correctness stress test
```

## A note on the third-party comparison

The original goal here was to benchmark against `boost::lockfree` or `moodycamel::ConcurrentQueue` directly. Neither is vendored into this repo, so the comparison baseline is a plain `std::mutex` + `std::deque` queue and a single global-mutex hash map instead. If you want the closer comparison, grab `moodycamel::ConcurrentQueue` (it's a single header) from its GitHub repo, or install `libboost-dev` for `boost::lockfree`, and add a third `runBenchmark<...>()` call in `queue_benchmark.cpp`. The benchmark harness is already generic over the queue type through the template parameter, so no other changes are needed.
