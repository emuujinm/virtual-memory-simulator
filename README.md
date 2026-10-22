# Core i7 MMU Simulator

A modular C++17 version of the original single-file Core i7 address-translation simulator.

## Project layout

```text
corei7_mmu/
├── include/
│   └── std_lib.hpp       # Standard-library includess
│   ├── const.hpp    # Architecture constants and PTE bit definitions
│   ├── algorithms.hpp   # VA/PA decomposition and replacement-policy selectors
│   ├── cache.hpp        # Cache and TLB classes
│   ├── mmu.hpp          # CoreI7MMU, physical memory, paging and translation
│   ├── result.hpp    # Access/statistics output
│   ├── tests.hpp        # Self-tests
│   └── workloads.hpp    # Workload generation, comparison and stdin mode
├── src/
│   └── main.cpp         # CLI dispatch and program entry point
├── CMakeLists.txt
├── Makefile
└── README.md
```

## Build

Using Make:

```sh
make
make test
./sim --demo
./sim --compare 256 20
./sim --stdin lru 64
./sim --maps <PID> lru 512 100000 random
```

Using CMake:

```sh
cmake -S . -B build
cmake --build build
./build/sim --test
```

For `--stdin`, provide one hexadecimal virtual address per line, optionally followed by `R` or `W`.

## Simulate replacement from a real process's memory map (Linux)

`--maps` parses `/proc/<PID>/maps`, takes readable virtual mapping ranges and permissions, and generates a synthetic virtual-page access trace for the existing MMU simulator:

```sh
./sim --maps <PID> lru 512 100000 random
./sim --maps <PID> fifo 512 100000 sequential
./sim --maps <PID> clock 512 100000 hot
```

Arguments are `PID [policy] [frames] [accesses] [pattern]`. Policies are `lru`, `fifo`, `clock`, and `random`; patterns are `sequential`, `random`, and `hot` (80% of references target the first 20% of collected pages). Defaults are LRU, 256 frames, 100,000 references, and random access. The target PID must exist and `/proc/<PID>/maps` must be readable by the current user.

**Important limitation:** `/proc/<PID>/maps` contains ranges and permissions, not a record of which addresses the process actually accesses. This mode does not inspect or modify target memory. Its page faults, evictions, and writebacks are results from synthetic references based on the real mapping layout, not the target process's real Linux paging behavior. For real access traces, instrument the process or use a tracing/profiling tool and feed the resulting address trace into `--stdin`.
