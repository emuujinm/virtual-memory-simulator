#pragma once
#include "std_lib.hpp"
#include "const.hpp"
#include "algorithms.hpp"

// Set-associative cache and translation

class Cache {
public:
    Cache(int sets, int ways, Repl r, uint32_t seed)
        : sets_(sets), ways_(ways), repl_(r), rng_(seed),
          lines_(static_cast<size_t>(sets) * ways) {}

    bool access(u64 pa) { // true on hit; fills the line on a miss
        u64 blk = pa >> 6;
        Line* s = &lines_[(blk % sets_) * ways_];
        u64 tag = blk / sets_;
        for (int w = 0; w < ways_; ++w)
            if (s[w].valid && s[w].tag == tag) { s[w].stamp = ++clock_; return true; }
        int v = -1;
        for (int w = 0; w < ways_; ++w) if (!s[w].valid) { v = w; break; }
        if (v < 0) {
            if (repl_ == Repl::Random) v = static_cast<int>(rng_() % ways_);
            else {
                v = 0;
                for (int w = 1; w < ways_; ++w) if (s[w].stamp < s[v].stamp) v = w;
            }
        }
        s[v] = {true, tag, ++clock_};
        return false;
    }
    void invalidate(u64 pa) {
        u64 blk = pa >> 6;
        Line* s = &lines_[(blk % sets_) * ways_];
        u64 tag = blk / sets_;
        for (int w = 0; w < ways_; ++w)
            if (s[w].valid && s[w].tag == tag) s[w].valid = false;
    }
private:
    struct Line { bool valid = false; u64 tag = 0, stamp = 0; };
    u64 sets_; int ways_;
    Repl repl_;
    std::mt19937 rng_;
    std::vector<Line> lines_;
    u64 clock_ = 0;
};

// TLB: 16 sets x 4 ways. Entries cache the PPN, effective R/W and U/S, and D.
struct TlbEntry {
    bool valid = false;
    u64 tag = 0, ppn = 0, pte_pa = 0, stamp = 0;
    bool rw = false, us = false, dirty = false;
};

class Tlb {
public:
    Tlb(Repl r, uint32_t seed) : repl_(r), rng_(seed) {}
    TlbEntry* lookup(u64 vpn) {
        TlbEntry* s = set_of(vpn);
        for (int w = 0; w < TLB_WAYS; ++w)
            if (s[w].valid && s[w].tag == (vpn >> 4)) { s[w].stamp = ++clock_; return &s[w]; }
        return nullptr;
    }
    void insert(u64 vpn, TlbEntry e) {
        TlbEntry* s = set_of(vpn);
        e.valid = true; e.tag = vpn >> 4; e.stamp = ++clock_;
        int v = -1;
        for (int w = 0; w < TLB_WAYS; ++w) if (!s[w].valid) { v = w; break; }
        if (v < 0) {
            if (repl_ == Repl::Random) v = static_cast<int>(rng_() % TLB_WAYS);
            else {
                v = 0;
                for (int w = 1; w < TLB_WAYS; ++w) if (s[w].stamp < s[v].stamp) v = w;
            }
        }
        s[v] = e;
    }
    void invalidate(u64 vpn) {
        TlbEntry* s = set_of(vpn);
        for (int w = 0; w < TLB_WAYS; ++w)
            if (s[w].valid && s[w].tag == (vpn >> 4)) s[w].valid = false;
    }
private:
    TlbEntry* set_of(u64 vpn) { return &entries_[(vpn & (TLB_SETS - 1)) * TLB_WAYS]; }
    Repl repl_;
    std::mt19937 rng_;
    std::array<TlbEntry, TLB_SETS * TLB_WAYS> entries_{};
    u64 clock_ = 0;
};


