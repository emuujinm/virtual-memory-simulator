#pragma once
#include "std_lib.hpp"
#include "const.hpp"
#include "algorithms.hpp"
#include "cache.hpp"

// Address translation, page tables, physical frames, page replacement
struct Latency { // cycles, illustrative
    int tlb = 1, l1 = 4, l2 = 12, l3 = 40, mem = 200;
    u64 fault = 1'000'000; // page-fault service (disk/SSD) time
};

struct Config {
    size_t frames = 256;
    PagePolicy page = PagePolicy::LRU;
    Repl tlb = Repl::LRU;
    Repl cache = Repl::LRU;
    uint32_t seed = 1;
    Latency lat;
};

struct Stats {
    u64 accesses = 0, tlb_hits = 0, tlb_misses = 0, pte_reads = 0;
    u64 page_faults = 0, prot_faults = 0, gp_faults = 0;
    u64 evictions = 0, dirty_writebacks = 0;
    u64 chit[3] = {0, 0, 0}, cmiss[3] = {0, 0, 0};
    u64 cycles = 0, fault_cycles = 0;
};

enum class Fault { None, NonCanonical, Protection };

struct Access {
    u64 va = 0, pa = 0;
    bool write = false, tlb_hit = false, walked = false, page_fault = false;
    Fault fault = Fault::None;
    int data_level = 0; // 1=L1, 2=L2, 3=L3, 4=DRAM
    int evictions = 0;
    u64 last_evicted_vpn = 0;
    bool last_evicted_dirty = false;
    u64 cycles = 0;
    u8 value = 0; // result of a load
};

class CoreI7MMU {
public:
    explicit CoreI7MMU(const Config& c)
        : cfg_(c), tlb_(c.tlb, c.seed * 7 + 1),
          l1_(L1_SETS, L1_WAYS, c.cache, c.seed * 7 + 2),
          l2_(L2_SETS, L2_WAYS, c.cache, c.seed * 7 + 3),
          l3_(L3_SETS, L3_WAYS, c.cache, c.seed * 7 + 4),
          rng_(c.seed * 7 + 5) {
        if (c.frames < 5) throw std::invalid_argument("need at least 5 frames (4 page-table pages + 1 data)");
        mem_.assign(c.frames * PAGE_SIZE, 0);
        info_.assign(c.frames, FrameInfo{});
        for (size_t f = c.frames; f > 0; --f) free_.push_back(static_cast<int>(f - 1));
        Access dummy;
        cr3_ = pa_of(alloc_frame(Kind::Table, 0, 0, dummy)); // L1 page table
    }

    Access load(u64 va) { return access(va, false, 0); }
    Access store(u64 va, u8 v) { return access(va, true, v); }

    const Stats& stats() const { return st_; }
    const Config& config() const { return cfg_; }
    u64 cr3() const { return cr3_; }
    size_t pinned_frames() const {
        size_t n = 0;
        for (const auto& f : info_) n += f.kind == Kind::Table;
        return n;
    }

    // Raw (side-effect free) views used by tests and the demo.
    u64 debug_read64(u64 pa) const { return rd64(pa); }

    u64 debug_l4_pte(u64 va) const {
        VaParts p = split_va(va);
        u64 t = cr3_;
        for (int L = 0; L < 3; ++L) {
            u64 pte = rd64(t + p.vpn[L] * 8);
            if (!(pte & PTE_P)) return 0;
            t = pte & PTE_ADDR_MASK;
        }
        return rd64(t + p.vpn[3] * 8);
    }

    // OS helper: change the R/W bit of a mapped page, then invalidate its TLB entry.
    bool protect_page(u64 va, bool writable) {
        VaParts p = split_va(va);
        u64 t = cr3_;
        for (int L = 0; L < 3; ++L) {
            u64 pte = rd64(t + p.vpn[L] * 8);
            if (!(pte & PTE_P)) return false;
            t = pte & PTE_ADDR_MASK;
        }
        u64 pa = t + p.vpn[3] * 8, pte = rd64(pa);
        if (!(pte & PTE_P)) return false;
        wr64(pa, writable ? (pte | PTE_RW) : (pte & ~PTE_RW));
        tlb_.invalidate(p.vpn_full);
        return true;
    }

    void dump_walk(u64 va, std::ostream& os) const {
        VaParts p = split_va(va);
        static const char* names[4] = {"L1 PTE (page global dir)  ", "L2 PTE (page upper dir)   ",
                                       "L3 PTE (page middle dir)  ", "L4 PTE (page table)       "};
        u64 t = cr3_;
        char buf[200];
        std::snprintf(buf, sizeof buf, "  CR3 = 0x%llx (physical address of L1 PT)\n",
                      (unsigned long long)cr3_);
        os << buf;
        for (int L = 0; L < LEVELS; ++L) {
            u64 pa = t + p.vpn[L] * 8, pte = rd64(pa);
            std::snprintf(buf, sizeof buf,
                          "  %s idx %3llu @ PA 0x%08llx = 0x%016llx  [%s]",
                          names[L], (unsigned long long)p.vpn[L], (unsigned long long)pa,
                          (unsigned long long)pte, flags(pte, L == 3).c_str());
            os << buf;
            if (!(pte & PTE_P)) { os << "  -> not present\n"; return; }
            std::snprintf(buf, sizeof buf, "  -> %s 0x%llx\n", L == 3 ? "PPN" : "next table PPN",
                          (unsigned long long)((pte & PTE_ADDR_MASK) >> PAGE_BITS));
            os << buf;
            t = pte & PTE_ADDR_MASK;
        }
    }

private:
    enum class Kind { Free, Table, Data };
    struct FrameInfo {
        Kind kind = Kind::Free;
        u64 vpn = 0, pte_pa = 0, load_time = 0, last_use = 0;
    };
    struct Walk {
        bool present = false, rw = true, us = true;
        u64 pte[LEVELS] = {0, 0, 0, 0}, pte_pa[LEVELS] = {0, 0, 0, 0}, ppn = 0;
    };

    static std::string flags(u64 pte, bool l4) {
        std::string s;
        auto add = [&](u64 b, const char* n) { if (pte & b) { if (!s.empty()) s += ' '; s += n; } };
        add(PTE_P, "P"); add(PTE_RW, "R/W"); add(PTE_US, "U/S"); add(PTE_A, "A");
        if (l4) add(PTE_D, "D"); else add(PTE_PS, "PS");
        return s;
    }

    u64 pa_of(int f) const { return (PPN_BASE + static_cast<u64>(f)) << PAGE_BITS; }
    size_t frame_of(u64 pa) const {
        u64 ppn = pa >> PAGE_BITS;
        if (ppn < PPN_BASE || ppn - PPN_BASE >= cfg_.frames)
            throw std::runtime_error("physical address outside installed memory");
        return static_cast<size_t>(ppn - PPN_BASE);
    }
    u64 rd64(u64 pa) const {
        u64 v; std::memcpy(&v, &mem_[frame_of(pa) * PAGE_SIZE + (pa & (PAGE_SIZE - 1))], 8); return v;
    }
    void wr64(u64 pa, u64 v) {
        std::memcpy(&mem_[frame_of(pa) * PAGE_SIZE + (pa & (PAGE_SIZE - 1))], &v, 8);
    }

    int cache_access(u64 pa, u64& cyc) {
        const Latency& L = cfg_.lat;
        if (l1_.access(pa)) { ++st_.chit[0]; cyc += L.l1; return 1; }
        ++st_.cmiss[0];
        if (l2_.access(pa)) { ++st_.chit[1]; cyc += L.l2; return 2; }
        ++st_.cmiss[1];
        if (l3_.access(pa)) { ++st_.chit[2]; cyc += L.l3; return 3; }
        ++st_.cmiss[2];
        cyc += L.mem;
        return 4;
    }

    // The hardware page walker: PTE reads go through the cache hierarchy
    Walk walk(const VaParts& p, u64& cyc) {
        Walk w;
        u64 table = cr3_;
        for (int L = 0; L < LEVELS; ++L) {
            u64 pte_pa = table + p.vpn[L] * 8;
            cache_access(pte_pa, cyc);
            ++st_.pte_reads;
            u64 pte = rd64(pte_pa);
            w.pte[L] = pte; w.pte_pa[L] = pte_pa;
            if (!(pte & PTE_P)) return w;                        // present stays false
            if ((L == 1 || L == 2) && (pte & PTE_PS))
                throw std::runtime_error("huge pages are not modelled");
            w.rw = w.rw && (pte & PTE_RW);
            w.us = w.us && (pte & PTE_US);
            table = pte & PTE_ADDR_MASK;
        }
        w.present = true;
        w.ppn = (w.pte[3] & PTE_ADDR_MASK) >> PAGE_BITS;
        return w;
    }

    Access access(u64 va, bool write, u8 wval) {
        Access r; r.va = va; r.write = write;
        ++st_.accesses;
        u64 cyc = 0;
        if (!canonical(va)) {
            r.fault = Fault::NonCanonical; ++st_.gp_faults;
            return r;
        }
        VaParts p = split_va(va);
        cyc += cfg_.lat.tlb;
        TlbEntry ent;
        bool ok;
        if (TlbEntry* e = tlb_.lookup(p.vpn_full)) { // TLB hit
            ++st_.tlb_hits; r.tlb_hit = true;
            ent = *e;
            ok = ent.us && (!write || ent.rw);
            if (ok && write && !ent.dirty) { // first write: set D
                wr64(ent.pte_pa, rd64(ent.pte_pa) | PTE_D);
                e->dirty = ent.dirty = true;
            }
        } else { // TLB miss
            ++st_.tlb_misses; r.walked = true;
            Walk w;
            for (int attempt = 0;; ++attempt) {
                w = walk(p, cyc);
                if (w.present) break;
                if (attempt >= 1) throw std::logic_error("page absent after fault handling");
                r.page_fault = true; ++st_.page_faults; // demand paging
                cyc += cfg_.lat.fault; st_.fault_cycles += cfg_.lat.fault;
                handle_fault(p, r);
            }
            ok = w.us && (!write || w.rw);
            if (ok) { // MMU sets A (and D)
                for (int L = 0; L < LEVELS; ++L) {
                    u64 v = w.pte[L] | PTE_A;
                    if (L == 3 && write) v |= PTE_D;
                    if (v != w.pte[L]) wr64(w.pte_pa[L], v);
                }
                ent.ppn = w.ppn; ent.pte_pa = w.pte_pa[3];
                ent.rw = w.rw; ent.us = w.us;
                ent.dirty = (w.pte[3] & PTE_D) || write;
                tlb_.insert(p.vpn_full, ent);
            }
        }
        if (!ok) {
            r.fault = Fault::Protection; ++st_.prot_faults;
            r.cycles = cyc; st_.cycles += cyc;
            return r;
        }
        r.pa = (ent.ppn << PAGE_BITS) | p.vpo;
        r.data_level = cache_access(r.pa, cyc);
        size_t f = frame_of(r.pa);
        info_[f].last_use = ++tick_;
        if (write) mem_[f * PAGE_SIZE + p.vpo] = wval;
        else       r.value = mem_[f * PAGE_SIZE + p.vpo];
        r.cycles = cyc; st_.cycles += cyc;
        return r;
    }

    // OS side: page-fault handler, frame allocator, replacement
    void handle_fault(const VaParts& p, Access& r) {
        u64 table = cr3_;
        for (int L = 0; L < 3; ++L) { // build missing page-table pages
            u64 pte_pa = table + p.vpn[L] * 8, pte = rd64(pte_pa);
            if (!(pte & PTE_P)) {
                int f = alloc_frame(Kind::Table, 0, 0, r);
                pte = (pa_of(f) & PTE_ADDR_MASK) | PTE_RW | PTE_US | PTE_P;
                wr64(pte_pa, pte);
            }
            table = pte & PTE_ADDR_MASK;
        }
        u64 pte_pa = table + p.vpn[3] * 8, pte = rd64(pte_pa);
        if (pte & PTE_P) return;
        int f = alloc_frame(Kind::Data, p.vpn_full, pte_pa, r);
        if (pte & PTE_OS_SWAPPED) { // page-in from the backing store
            auto it = backing_.find(p.vpn_full);
            if (it != backing_.end())
                std::memcpy(&mem_[static_cast<size_t>(f) * PAGE_SIZE], it->second.data(), PAGE_SIZE);
        } // else: zero-fill (frame is zeroed)
        u64 prot = pte == 0 ? (PTE_RW | PTE_US) : (pte & (PTE_RW | PTE_US));
        wr64(pte_pa, (pa_of(f) & PTE_ADDR_MASK) | prot | PTE_P);   // A = 0, D = 0
    }

    int alloc_frame(Kind kind, u64 vpn, u64 pte_pa, Access& r) {
        int f;
        if (!free_.empty()) { f = free_.back(); free_.pop_back(); }
        else f = evict_one(r);
        std::memset(&mem_[static_cast<size_t>(f) * PAGE_SIZE], 0, PAGE_SIZE);
        for (u64 o = 0; o < PAGE_SIZE; o += LINE) {    // DMA-style: drop stale cache lines
            l1_.invalidate(pa_of(f) + o); l2_.invalidate(pa_of(f) + o); l3_.invalidate(pa_of(f) + o);
        }
        info_[f] = {kind, vpn, pte_pa, ++tick_, tick_};
        return f;
    }

    int evict_one(Access& r) {
        int f = pick_victim();
        FrameInfo& fi = info_[f];
        u64 pte = rd64(fi.pte_pa);
        r.last_evicted_dirty = (pte & PTE_D) != 0;
        if (pte & PTE_D) { // write back dirty page
            auto& b = backing_[fi.vpn];
            b.assign(&mem_[static_cast<size_t>(f) * PAGE_SIZE],
                     &mem_[static_cast<size_t>(f) * PAGE_SIZE] + PAGE_SIZE);
            ++st_.dirty_writebacks;
        }
        wr64(fi.pte_pa, (pte & (PTE_RW | PTE_US)) | PTE_OS_SWAPPED); // P = A = D = 0
        tlb_.invalidate(fi.vpn); // invlpg
        r.last_evicted_vpn = fi.vpn;
        ++r.evictions; ++st_.evictions;
        fi.kind = Kind::Free;
        return f;
    }

    int pick_victim() {
        const size_t N = cfg_.frames;
        if (cfg_.page == PagePolicy::CLOCK) { // second chance on the hardware A bit
            for (size_t guard = 0; guard < 2 * N + 2; ++guard) {
                size_t f = hand_;
                hand_ = (hand_ + 1) % N;
                if (info_[f].kind != Kind::Data) continue;
                u64 pte = rd64(info_[f].pte_pa);
                if (pte & PTE_A) {
                    wr64(info_[f].pte_pa, pte & ~PTE_A);
                    tlb_.invalidate(info_[f].vpn); // so the next access sets A again
                    continue;
                }
                return static_cast<int>(f);
            }
            throw std::runtime_error("no evictable frame (all pinned)");
        }
        std::vector<int> cand;
        for (size_t f = 0; f < N; ++f) if (info_[f].kind == Kind::Data) cand.push_back(static_cast<int>(f));
        if (cand.empty()) throw std::runtime_error("no evictable frame (all pinned)");
        switch (cfg_.page) {
            case PagePolicy::FIFO:
                return *std::min_element(cand.begin(), cand.end(),
                    [&](int a, int b) { return info_[a].load_time < info_[b].load_time; });
            case PagePolicy::LRU:
                return *std::min_element(cand.begin(), cand.end(),
                    [&](int a, int b) { return info_[a].last_use < info_[b].last_use; });
            case PagePolicy::RANDOM:
                return cand[rng_() % cand.size()];
            default: break;
        }
        throw std::logic_error("unreachable");
    }

    Config cfg_;
    Tlb tlb_;
    Cache l1_, l2_, l3_;
    std::mt19937 rng_;
    std::vector<u8> mem_;
    std::vector<FrameInfo> info_;
    std::vector<int> free_;
    std::unordered_map<u64, std::vector<u8>> backing_; // swap: vpn -> page contents
    u64 cr3_ = 0, tick_ = 0;
    size_t hand_ = 0;
    Stats st_;
};
