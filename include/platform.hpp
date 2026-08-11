#pragma once
// Cross-platform "spin-wait hint" instruction for x86_64 and ARM64 (e.g.
// Apple Silicon). Used by Backoff in mpmc_queue.hpp between failed CAS
// retries: it tells the CPU "I'm in a busy-wait loop," which reduces
// speculative memory traffic and gives cache-coherence protocol time to
// settle before the next retry hammers the same line again.
namespace hft {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm64__)
    asm volatile("yield" ::: "memory");
#else
    asm volatile("" ::: "memory"); // generic compiler barrier fallback
#endif
}

} // namespace hft
