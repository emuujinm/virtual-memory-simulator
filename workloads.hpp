#pragma once
#include "std_lib.hpp"
#include "const.hpp"
#include "algorithms.hpp"
#include "mmu.hpp"
#include "result.hpp"

// Workload generation, policy comparison, and trace-driven mode.

// Demo: shows each stage of the handout's end-to-end translation

static void demo() {
    std::puts("\nDemo: Core i7 end-to-end translation (16 frames, LRU page replacement)");
    std::puts("Columns: VA fields | TLB | PA fields | L1 cache fields -> level that served the data\n");
    Config c; c.frames = 16; c.page = PagePolicy::LRU;
    CoreI7MMU m(c);
    struct R { u64 va; bool w; };
    std::vector<R> refs = {
        {0x00007f5a12345678ull, true},  {0x00007f5a12345678ull, false}, {0x00007f5a12345680ull, false},
        {0x0000000000400100ull, false}, {0x00007ffcffffeff8ull, true},
    };
    for (int i = 1; i <= 9; ++i) refs.push_back({0x00007f5a20000000ull + i * PAGE_SIZE, false});  // force evictions
    refs.push_back({0x00007f5a12345678ull, false});   // was evicted: page-in from backing store
    for (size_t i = 0; i < refs.size(); ++i) {
        Access a = refs[i].w ? m.store(refs[i].va, 0xAB) : m.load(refs[i].va);
        print_access(a);
        if (i == 0) {
            std::puts("   page walk performed by the first access (PTEs now in simulated physical memory):");
            m.dump_walk(refs[i].va, std::cout);
        }
    }
    std::printf("\nValue read back from the page that was evicted and paged in again: 0x%02X (wrote 0xAB)\n",
                m.load(0x00007f5a12345678ull).value);
    print_stats(m.stats());
}

// Workloads and comparison experiment
struct Op { u64 va; bool write; };
struct Workload {
    std::string name, note;
    std::vector<Op> ops;
    Workload(std::string n, std::string d) : name(std::move(n)), note(std::move(d)) {}
};

static std::vector<Workload> make_workloads(size_t F) {
    std::mt19937_64 g(2024);
    const u64 HEAP = 0x00007f5a10000000ull, TEXT = 0x400000ull, STACK_TOP = 0x00007ffcffffe000ull;
    auto pva = [](u64 base, u64 page, u64 off) { return base + page * PAGE_SIZE + off; };
    std::vector<Workload> out;
    const size_t N = 60000;

    {   // cyclic scan slightly larger than memory
        Workload w{"loop", "cyclic scan of 1.25x memory"};
        u64 pages = F * 5 / 4;
        for (int pass = 0; pass < 12; ++pass)
            for (u64 p = 0; p < pages; ++p) {
                w.ops.push_back({pva(HEAP, p, 0), p % 4 == 0});
                w.ops.push_back({pva(HEAP, p, 2048), false});
            }
        out.push_back(std::move(w));
    }
    {   // 90% of accesses to a hot set of 0.5x memory, 10% to a cold set of 4x memory
        Workload w{"hot/cold", "90% hot (0.5x mem), 10% cold (4x mem)"};
        for (size_t i = 0; i < N; ++i) {
            bool hot = g() % 100 < 90;
            u64 p = hot ? g() % (F / 2) : F / 2 + g() % (F * 4);
            w.ops.push_back({pva(HEAP, p, g() % PAGE_SIZE), g() % 4 == 0});
        }
        out.push_back(std::move(w));
    }
    {   // sliding working set
        Workload w{"phases", "working set 0.75x mem sliding by 25% every 3000 refs"};
        u64 ws = F * 3 / 4, base = 0;
        for (size_t i = 0; i < N; ++i) {
            if (i && i % 3000 == 0) base += ws / 4;
            w.ops.push_back({pva(HEAP, base + g() % ws, g() % PAGE_SIZE), g() % 5 == 0});
        }
        out.push_back(std::move(w));
    }
    {   // uniform random over 2x memory
        Workload w{"uniform", "uniform random over 2x memory"};
        for (size_t i = 0; i < N; ++i)
            w.ops.push_back({pva(HEAP, g() % (F * 2), g() % PAGE_SIZE), g() % 5 == 0});
        out.push_back(std::move(w));
    }
    {   // Zipf(1.0) over 4x memory, ranks scattered across the address space
        Workload w{"zipf", "Zipf(s=1) over 4x memory"};
        size_t n = F * 4;
        std::vector<double> cdf(n);
        double tot = 0;
        for (size_t r = 0; r < n; ++r) cdf[r] = (tot += 1.0 / double(r + 1));
        std::vector<u64> perm(n);
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), g);
        std::uniform_real_distribution<double> u(0.0, tot);
        for (size_t i = 0; i < N; ++i) {
            size_t r = std::lower_bound(cdf.begin(), cdf.end(), u(g)) - cdf.begin();
            w.ops.push_back({pva(HEAP, perm[std::min(r, n - 1)], g() % PAGE_SIZE), g() % 5 == 0});
        }
        out.push_back(std::move(w));
    }
    {   // text loop + zipf heap + sliding stack: exercises several page-table subtrees
        Workload w{"mixed", "text loop + Zipf heap + stack window (3 regions)"};
        size_t n = F * 3;
        std::vector<double> cdf(n);
        double tot = 0;
        for (size_t r = 0; r < n; ++r) cdf[r] = (tot += 1.0 / double(r + 1));
        std::uniform_real_distribution<double> u(0.0, tot);
        u64 pc = 0;
        for (size_t i = 0; i < N; ++i) {
            u64 k = g() % 10;
            if (k < 2)      { w.ops.push_back({pva(TEXT, pc++ % 24, (i * 64) % PAGE_SIZE), false}); }
            else if (k < 8) {
                size_t r = std::lower_bound(cdf.begin(), cdf.end(), u(g)) - cdf.begin();
                w.ops.push_back({pva(HEAP, std::min(r, n - 1), g() % PAGE_SIZE), g() % 4 == 0});
            } else {
                u64 depth = (i / 500) % 40 + g() % 8;
                w.ops.push_back({STACK_TOP - depth * PAGE_SIZE + g() % PAGE_SIZE, true});
            }
        }
        out.push_back(std::move(w));
    }
    return out;
}

struct Outcome { Stats st; u64 bad = 0; };

static Outcome run_ops(const std::vector<Op>& ops, const Config& cfg) {
    CoreI7MMU m(cfg);
    std::unordered_map<u64, u8> shadow;
    Outcome o;
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (op.write) {
            u8 v = static_cast<u8>(i * 131 + 7);
            if (m.store(op.va, v).fault == Fault::None) shadow[op.va] = v;
        } else {
            u8 e = shadow.count(op.va) ? shadow[op.va] : 0;
            o.bad += m.load(op.va).value != e;
        }
    }
    o.st = m.stats();
    return o;
}

struct Agg { // mean/stddev of a metric over seeds
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
    double sd() const {
        if (v.size() < 2) return 0;
        double m = mean(), s = 0;
        for (double x : v) s += (x - m) * (x - m);
        return std::sqrt(s / (v.size() - 1));
    }
};

static double pct(u64 a, u64 b) { return a + b ? 100.0 * a / (a + b) : 0.0; }

static void compare(size_t frames, int seeds) {
    std::printf("\nComparison: %zu physical frames, %d seeds for Random, TLB 16x4, L1/L2/L3 per handout\n",
                frames, seeds);
    std::puts("Every run verifies each loaded byte against a shadow copy (mismatches must be 0).");
    auto wls = make_workloads(frames);
    u64 total_bad = 0;

    std::puts("\n=== Table 1: page replacement (TLB and caches use LRU) ===");
    std::puts("workload   policy        faults (mean +- sd)  fault%  dirty-wb  TLB hit%  avg cyc/access");
    for (const auto& w : wls) {
        std::printf("-- %s: %s, %zu references\n", w.name.c_str(), w.note.c_str(), w.ops.size());
        const PagePolicy det[] = {PagePolicy::FIFO, PagePolicy::LRU, PagePolicy::CLOCK};
        struct Row { std::string name; double faults, sd, wb, tlb, cyc, rate; };
        std::vector<Row> rows;
        for (PagePolicy p : det) {
            Config c; c.frames = frames; c.page = p;
            Outcome o = run_ops(w.ops, c);
            total_bad += o.bad;
            double a = double(o.st.accesses);
            rows.push_back({name(p), double(o.st.page_faults), 0, double(o.st.dirty_writebacks),
                            100.0 * o.st.tlb_hits / a, o.st.cycles / a, 100.0 * o.st.page_faults / a});
        }
        Agg f, wb, tl, cy;
        for (int s = 1; s <= seeds; ++s) {
            Config c; c.frames = frames; c.page = PagePolicy::RANDOM; c.seed = static_cast<uint32_t>(s);
            Outcome o = run_ops(w.ops, c);
            total_bad += o.bad;
            double a = double(o.st.accesses);
            f.add(double(o.st.page_faults)); wb.add(double(o.st.dirty_writebacks));
            tl.add(100.0 * o.st.tlb_hits / a); cy.add(o.st.cycles / a);
        }
        rows.push_back({"Random", f.mean(), f.sd(), wb.mean(), tl.mean(), cy.mean(),
                        100.0 * f.mean() / double(w.ops.size())});
        size_t best = 0;
        for (size_t i = 1; i < rows.size(); ++i) if (rows[i].faults < rows[best].faults) best = i;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            std::printf("           %-6s %s  %9.0f +- %-7.0f  %5.1f   %8.0f   %6.1f   %10.1f\n",
                        r.name.c_str(), i == best ? "*" : " ", r.faults, r.sd, r.rate, r.wb, r.tlb, r.cyc);
        }
    }
    std::puts("(* = fewest page faults)");

    std::puts("\n=== Table 2: TLB + cache replacement, LRU vs Random (page policy LRU) ===");
    std::puts("workload   TLB/cache  TLB hit%   L1 hit%  L2 hit%  L3 hit%   cyc/access excl. faults");
    for (const auto& w : wls) {
        for (int mode = 0; mode < 2; ++mode) {
            Agg th, l1, l2, l3, cy;
            int runs = mode == 0 ? 1 : seeds;
            for (int s = 1; s <= runs; ++s) {
                Config c; c.frames = frames; c.page = PagePolicy::LRU;
                c.tlb = c.cache = mode == 0 ? Repl::LRU : Repl::Random;
                c.seed = static_cast<uint32_t>(s);
                Outcome o = run_ops(w.ops, c);
                total_bad += o.bad;
                double a = double(o.st.accesses);
                th.add(100.0 * o.st.tlb_hits / a);
                l1.add(pct(o.st.chit[0], o.st.cmiss[0])); l2.add(pct(o.st.chit[1], o.st.cmiss[1]));
                l3.add(pct(o.st.chit[2], o.st.cmiss[2]));
                cy.add((o.st.cycles - o.st.fault_cycles) / a);
            }
            std::printf("%-10s %-9s  %6.2f    %6.2f   %6.2f   %6.2f    %8.2f\n",
                        mode == 0 ? w.name.c_str() : "", mode == 0 ? "LRU" : "Random",
                        th.mean(), l1.mean(), l2.mean(), l3.mean(), cy.mean());
        }
    }
    std::printf("\nData-integrity mismatches across all runs: %llu\n", (unsigned long long)total_bad);
}

// /proc/<pid>/maps-driven synthetic workload
struct MapRegion {
    u64 start = 0, end = 0;
    bool readable = false, writable = false, executable = false;
    std::string name;
};

static std::vector<MapRegion> read_proc_maps(int pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path + "; check PID and permissions (Linux only)");
    std::vector<MapRegion> regions;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream is(line);
        std::string range, perms, offset, dev, inode;
        if (!(is >> range >> perms >> offset >> dev >> inode)) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        MapRegion r;
        try {
            r.start = std::stoull(range.substr(0, dash), nullptr, 16);
            r.end = std::stoull(range.substr(dash + 1), nullptr, 16);
        } catch (...) { continue; }
        if (r.end <= r.start) continue;
        r.readable = perms.size() > 0 && perms[0] == 'r';
        r.writable = perms.size() > 1 && perms[1] == 'w';
        r.executable = perms.size() > 2 && perms[2] == 'x';
        std::getline(is, r.name);
        const auto first = r.name.find_first_not_of(" \t");
        r.name = first == std::string::npos ? "[anonymous]" : r.name.substr(first);
        regions.push_back(std::move(r));
    }
    if (regions.empty()) throw std::runtime_error("no readable mapping records found in " + path);
    return regions;
}

struct MappedPage { u64 va; bool writable; };

static std::vector<MappedPage> collect_mapped_pages(const std::vector<MapRegion>& regions,
                                                     size_t max_pages = 1'000'000) {
    std::vector<MappedPage> pages;
    for (const auto& r : regions) {
        if (!r.readable) continue; // generate accesses only to readable regions
        u64 first = r.start & ~(u64(PAGE_SIZE) - 1);
        u64 last = (r.end - 1) & ~(u64(PAGE_SIZE) - 1);
        for (u64 va = first;; va += PAGE_SIZE) {
            if (pages.size() >= max_pages)
                throw std::runtime_error("more than 1,000,000 readable mapped pages; refusing an oversized trace model");
            pages.push_back({va, r.writable});
            if (va == last || va > std::numeric_limits<u64>::max() - PAGE_SIZE) break;
        }
    }
    if (pages.empty()) throw std::runtime_error("target process has no readable mapped pages");
    return pages;
}

static void run_proc_maps(int pid, PagePolicy pol, size_t frames, size_t accesses,
                          const std::string& pattern) {
    auto regions = read_proc_maps(pid);
    auto pages = collect_mapped_pages(regions);
    if (frames < 5) throw std::invalid_argument("frames must be at least 5");
    if (accesses == 0) throw std::invalid_argument("access count must be greater than zero");
    if (pattern != "sequential" && pattern != "random" && pattern != "hot")
        throw std::invalid_argument("pattern must be sequential, random, or hot");

    size_t writable_pages = 0;
    for (const auto& p : pages) writable_pages += p.writable;
    std::mt19937_64 rng(0x4d415053u + static_cast<uint64_t>(pid));
    std::uniform_int_distribution<size_t> all(0, pages.size() - 1);
    std::uniform_int_distribution<size_t> hot(0, std::max<size_t>(1, pages.size() / 5) - 1);

    Config c; c.frames = frames; c.page = pol; c.seed = static_cast<uint32_t>(pid > 0 ? pid : 1);
    CoreI7MMU mmu(c);
    std::printf("\n/proc/%d/maps page-replacement simulation\n", pid);
    std::printf("Readable regions: %zu | readable mapped pages: %zu | writable pages: %zu\n",
                std::count_if(regions.begin(), regions.end(), [](const MapRegion& r) { return r.readable; }),
                pages.size(), writable_pages);
    std::printf("Policy: %s | simulated frames: %zu | references: %zu | pattern: %s\n",
                name(pol), frames, accesses, pattern.c_str());
    std::puts("NOTE: only mapping addresses/permissions are read. The target process is not read or modified; accesses are synthetic.");

    size_t cursor = 0;
    for (size_t i = 0; i < accesses; ++i) {
        size_t idx;
        if (pattern == "sequential") idx = cursor++ % pages.size();
        else if (pattern == "hot" && (rng() % 100 < 80)) idx = hot(rng);
        else idx = all(rng);
        const auto& page = pages[idx];
        // Keep the synthetic byte address within the selected page.
        u64 va = page.va + ((i * 64u) % PAGE_SIZE);
        bool write = page.writable && (rng() % 5 == 0);
        if (write) (void)mmu.store(va, static_cast<u8>(i * 31u + 9u));
        else (void)mmu.load(va);
    }
    print_stats(mmu.stats());
    std::puts("Interpretation: these page faults/evictions are simulator results for the synthetic trace, not the target process's real Linux page-fault counters.");
}

// Trace-driven mode
static void run_stdin(PagePolicy pol, size_t frames) {
    Config c; c.page = pol; c.frames = frames;
    CoreI7MMU m(c);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string a, rw;
        if (!(is >> a)) continue;
        is >> rw;
        bool w = !rw.empty() && (rw[0] == 'W' || rw[0] == 'w');
        u64 va = std::stoull(a, nullptr, 16);
        print_access(w ? m.store(va, 0) : m.load(va));
    }
    print_stats(m.stats());
}

