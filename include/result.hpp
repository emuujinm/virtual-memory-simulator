#pragma once
#include "std_lib.hpp"
#include "const.hpp"
#include "algorithms.hpp"
#include "mmu.hpp"

// Formatted result

static void print_access(const Access& a) {
    char b[400];
    if (a.fault == Fault::NonCanonical) {
        std::printf("VA 0x%016llx %c | #GP non-canonical address\n", (unsigned long long)a.va, a.write ? 'W' : 'R');
        return;
    }
    VaParts p = split_va(a.va);
    int n = std::snprintf(b, sizeof b,
        "VA 0x%016llx %c | VPN1-4 %03llx %03llx %03llx %03llx VPO %03llx | TLBT %08llx TLBI %llx %s",
        (unsigned long long)a.va, a.write ? 'W' : 'R',
        (unsigned long long)p.vpn[0], (unsigned long long)p.vpn[1], (unsigned long long)p.vpn[2],
        (unsigned long long)p.vpn[3], (unsigned long long)p.vpo,
        (unsigned long long)p.tlbt, (unsigned long long)p.tlbi, a.tlb_hit ? "HIT " : "MISS");
    std::fputs(b, stdout);
    if (a.page_fault) std::fputs(" +PAGE-FAULT", stdout);
    if (a.fault == Fault::Protection) { std::puts(" | #PF protection violation"); return; }
    PaParts q = split_pa(a.pa);
    static const char* lv[5] = {"", "L1", "L2", "L3", "DRAM"};
    std::snprintf(b, sizeof b, " | PPN %llx PPO %03llx PA 0x%llx | CT %llx CI %02llx CO %02llx -> %s | %llu cyc",
                  (unsigned long long)q.ppn, (unsigned long long)q.ppo, (unsigned long long)a.pa,
                  (unsigned long long)q.ct, (unsigned long long)q.ci, (unsigned long long)q.co,
                  lv[a.data_level], (unsigned long long)a.cycles);
    (void)n;
    std::fputs(b, stdout);
    if (a.evictions) std::printf(" | evicted VPN 0x%llx%s", (unsigned long long)a.last_evicted_vpn,
                                 a.last_evicted_dirty ? " (dirty, written back)" : "");
    std::puts("");
}

static void print_stats(const Stats& s) {
    double acc = s.accesses ? double(s.accesses) : 1.0;
    auto rate = [](u64 h, u64 m) { return h + m ? 100.0 * h / (h + m) : 0.0; };
    std::printf("accesses %llu | TLB hit %.1f%% | page walks %llu (%llu PTE reads) | page faults %llu"
                " | evictions %llu (dirty %llu)\n",
                (unsigned long long)s.accesses, 100.0 * s.tlb_hits / acc,
                (unsigned long long)s.tlb_misses, (unsigned long long)s.pte_reads,
                (unsigned long long)s.page_faults, (unsigned long long)s.evictions,
                (unsigned long long)s.dirty_writebacks);
    std::printf("cache hit rates: L1 %.1f%%  L2 %.1f%%  L3 %.1f%% | avg %.1f cycles/access\n",
                rate(s.chit[0], s.cmiss[0]), rate(s.chit[1], s.cmiss[1]), rate(s.chit[2], s.cmiss[2]),
                s.cycles / acc);
}
