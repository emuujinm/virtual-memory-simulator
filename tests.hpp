#pragma once
#include "std_lib.hpp"
#include "const.hpp"
#include "algorithms.hpp"
#include "mmu.hpp"

// Self-tests for address translation, protection, and paging

static int g_fail = 0;
#define CHECK(cond, msg)                                                          \
    do { if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }             \
         else std::printf("  ok:   %s\n", msg); } while (0)

static u64 manual_translate(const CoreI7MMU& m, u64 va) { // independent walk
    u64 table = m.cr3();
    for (int L = 0; L < 4; ++L) {
        int shift = 12 + 9 * (3 - L);
        u64 idx = (va >> shift) & 0x1FF;
        u64 pte = m.debug_read64(table + idx * 8);
        if (!(pte & 1)) return ~0ull;
        table = pte & 0x000FFFFFFFFFF000ull;
    }
    return table | (va & 0xFFF);
}

static u64 faults_for(PagePolicy pol, size_t data_frames, const std::vector<u64>& pages) {
    Config c; c.page = pol; c.frames = 4 + data_frames; // 4 pinned: L1..L4 tables
    CoreI7MMU m(c);
    for (u64 p : pages) m.load(p << PAGE_BITS);
    return m.stats().page_faults;
}

static bool self_test() {
    std::puts("Self-tests:");
    // 1. Field extraction against hand-composed values
    {
        u64 va = (1ull << 39) | (2ull << 30) | (3ull << 21) | (4ull << 12) | 5;
        VaParts p = split_va(va);
        CHECK(p.vpn[0] == 1 && p.vpn[1] == 2 && p.vpn[2] == 3 && p.vpn[3] == 4 && p.vpo == 5,
              "VPN1..VPN4 and VPO extracted (9/9/9/9/12)");
        VaParts t = split_va(0x123456789ull << 12);
        CHECK(t.tlbi == 0x9 && t.tlbt == 0x12345678, "TLBI = low 4 bits, TLBT = upper 32 bits of VPN");
        PaParts q = split_pa(0x12345678);
        CHECK(q.co == 0x38 && q.ci == 0x19 && q.ct == 0x12345 && q.ppo == 0x678,
              "PA split into CT/CI/CO (40/6/6) and PPN/PPO");
        CHECK(canonical(0x00007fffffffffffull) && canonical(0xffff800000000000ull) &&
              !canonical(0x0000800000000000ull) && !canonical(0x0001000000000000ull),
              "canonical-address check");
    }
    // 2. End-to-end translation vs independent walk, plus data round trip
    {
        Config c; c.frames = 64;
        CoreI7MMU m(c);
        const u64 vas[] = {0x400abc, 0x1, 0x00007f5a12345678ull, 0x00007ffcffffeff8ull, 0x00007fffffffffffull};
        bool all = true, roundtrip = true, hit = true, ppo = true;
        for (u64 va : vas) {
            Access s = m.store(va, static_cast<u8>(va * 31 + 7));
            all = all && s.pa == manual_translate(m, va) && s.fault == Fault::None;
            ppo = ppo && (s.pa & 0xFFF) == (va & 0xFFF);
            Access l = m.load(va);
            roundtrip = roundtrip && l.value == static_cast<u8>(va * 31 + 7);
            hit = hit && l.tlb_hit && !l.page_fault;
        }
        CHECK(all, "MMU physical address == independent 4-level walk from CR3");
        CHECK(ppo, "PPO equals VPO");
        CHECK(roundtrip, "store then load returns the stored byte");
        CHECK(hit, "second access to a page hits the TLB with no fault");
        Access bad = m.load(0x0001000000000000ull);
        CHECK(bad.fault == Fault::NonCanonical, "non-canonical address rejected");
    }
    // 3. A/D bits and protection
    {
        Config c; c.frames = 32;
        CoreI7MMU m(c);
        m.load(0x5000); m.store(0x6000, 1);
        u64 a = m.debug_l4_pte(0x5000), b = m.debug_l4_pte(0x6000);
        CHECK((a & PTE_A) && !(a & PTE_D), "read sets A only");
        CHECK((b & PTE_A) && (b & PTE_D), "write sets A and D");
        m.load(0x7000);                         // TLB entry loaded clean
        m.store(0x7008, 9);                     // write hit must still set D
        CHECK(m.debug_l4_pte(0x7000) & PTE_D, "write on a clean TLB entry sets D in the PTE");
        m.protect_page(0x6000, false);
        CHECK(m.store(0x6000, 2).fault == Fault::Protection, "write to read-only page faults");
        CHECK(m.load(0x6000).fault == Fault::None && m.load(0x6000).value == 1, "read of read-only page works");
        m.protect_page(0x6000, true);
        CHECK(m.store(0x6000, 3).fault == Fault::None, "write allowed again after restoring R/W");
    }
    // 4. Known page-replacement results (data frames = total - 4 pinned table frames)
    {
        const std::vector<u64> belady = {1,2,3,4,1,2,5,1,2,3,4,5};
        const std::vector<u64> text   = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};
        u64 f3 = faults_for(PagePolicy::FIFO, 3, belady), f4 = faults_for(PagePolicy::FIFO, 4, belady);
        CHECK(f3 == 9 && f4 == 10, "FIFO Belady anomaly: 9 faults (3 frames), 10 faults (4 frames)");
        CHECK(faults_for(PagePolicy::FIFO, 3, text) == 15, "FIFO textbook string, 3 frames = 15 faults");
        CHECK(faults_for(PagePolicy::LRU, 3, text) == 12, "LRU textbook string, 3 frames = 12 faults");
    }
    // 5. Data integrity under heavy paging for every policy
    {
        const PagePolicy pols[] = {PagePolicy::FIFO, PagePolicy::LRU, PagePolicy::CLOCK, PagePolicy::RANDOM};
        for (PagePolicy pol : pols) {
            Config c; c.frames = 24; c.page = pol;
            CoreI7MMU m(c);
            std::mt19937_64 g(5);
            std::unordered_map<u64, u8> shadow;
            u64 bad = 0;
            for (int i = 0; i < 20000; ++i) {
                u64 va = 0x00007f0000000000ull + (g() % 200) * PAGE_SIZE + (g() % PAGE_SIZE);
                if (g() & 1) { u8 v = static_cast<u8>(g()); m.store(va, v); shadow[va] = v; }
                else { u8 e = shadow.count(va) ? shadow[va] : 0; bad += m.load(va).value != e; }
            }
            char msg[100];
            std::snprintf(msg, sizeof msg, "%s: 20000 mixed accesses over 200 pages in ~19 frames, 0 mismatches",
                          name(pol));
            CHECK(bad == 0 && m.stats().page_faults > 100 && m.stats().dirty_writebacks > 0, msg);
        }
    }
    std::printf("%s\n", g_fail ? "SELF-TEST FAILED" : "All self-tests passed.");
    return g_fail == 0;
}
