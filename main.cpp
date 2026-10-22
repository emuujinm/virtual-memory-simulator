#include "const.hpp"
#include "algorithms.hpp"
#include "cache.hpp"
#include "mmu.hpp"
#include "result.hpp"
#include "tests.hpp"
#include "workloads.hpp"

int main(int argc, char** argv) {
    try {
        std::string mode = argc > 1 ? argv[1] : "--all";
        if (mode == "--test")    return self_test() ? 0 : 1;
        if (mode == "--demo")    { demo(); return 0; }
        if (mode == "--compare") {
            compare(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 256,
                    argc > 3 ? std::atoi(argv[3]) : 20);
            return 0;
        }
        if (mode == "--stdin") {
            std::string p = argc > 2 ? argv[2] : "lru";
            PagePolicy pol = p == "fifo" ? PagePolicy::FIFO : p == "clock" ? PagePolicy::CLOCK
                           : p == "random" ? PagePolicy::RANDOM : PagePolicy::LRU;
            run_stdin(pol, argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 64);
            return 0;
        }
        if (mode == "--maps") {
            if (argc < 3) throw std::invalid_argument("usage: --maps PID [lru|fifo|clock|random] [frames] [accesses] [sequential|random|hot]");
            int pid = std::atoi(argv[2]);
            if (pid <= 0) throw std::invalid_argument("PID must be a positive integer");
            std::string p = argc > 3 ? argv[3] : "lru";
            PagePolicy pol;
            if (p == "fifo") pol = PagePolicy::FIFO;
            else if (p == "clock") pol = PagePolicy::CLOCK;
            else if (p == "random") pol = PagePolicy::RANDOM;
            else if (p == "lru") pol = PagePolicy::LRU;
            else throw std::invalid_argument("policy must be lru, fifo, clock, or random");
            size_t frames = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 256;
            size_t accesses = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 100000;
            std::string pattern = argc > 6 ? argv[6] : "random";
            run_proc_maps(pid, pol, frames, accesses, pattern);
            return 0;
        }
        if (mode == "--all") {
            if (!self_test()) return 1;
            demo();
            compare(256, 20);
            return 0;
        }
        std::fprintf(stderr, "usage: %s [--test|--demo|--compare [frames] [seeds]|--stdin [policy] [frames]|--maps PID [policy] [frames] [accesses] [pattern]]\n", argv[0]);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
