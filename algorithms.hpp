#pragma once
#include "std_lib.hpp"

// Page replacement algorithms

struct VaParts {
    u64 vpn[LEVELS]; // VPN[0...3]
    u64 vpn_full; // 36-bit
    u64 vpo; // 12-bit
    u64 tlbt; // 32-bit tag
    u64 tlbi; // 4-bit index
};

inline bool canonical(u64 va) {
    return static_cast<int64_t>(va << 16) >> 16 == static_cast<int64_t>(va);
}

inline VaParts split_va(u64 va) {
    VaParts p{};
    p.vpo = va & (PAGE_SIZE - 1);
    p.vpn_full = (va >> PAGE_BITS) & ((1ull << 36) - 1);
    for (int i = 0; i < LEVELS; ++i)
        p.vpn[i] = (p.vpn_full >> (LEVEL_BITS * (LEVELS - 1 - i))) & (ENTRIES - 1);
    p.tlbi = p.vpn_full & (TLB_SETS - 1);
    p.tlbt = p.vpn_full >> 4;
    return p;
}

struct PaParts { u64 ppn, ppo, ct, ci, co; }; // L1 view of a physical address
inline PaParts split_pa(u64 pa) {
    return {pa >> PAGE_BITS, pa & (PAGE_SIZE - 1), pa >> 12,
            (pa >> 6) & (L1_SETS - 1), pa & (LINE - 1)};
}

// Replacement policy selectors
enum class Repl { LRU, Random }; // TLB and caches
enum class PagePolicy { FIFO, LRU, CLOCK, RANDOM }; // physical frames

inline const char* name(Repl r) { return r == Repl::LRU ? "LRU" : "Random"; }
inline const char* name(PagePolicy p) {
    switch (p) {
        case PagePolicy::FIFO:   return "FIFO";
        case PagePolicy::LRU:    return "LRU";
        case PagePolicy::CLOCK:  return "Clock";
        case PagePolicy::RANDOM: return "Random";
    }
    return "?";
}

