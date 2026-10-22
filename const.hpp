#pragma once
#include "std_lib.hpp"

// Architecture constants and page-table entry bit definitions.
using u64 = uint64_t;
using u8  = uint8_t;

constexpr int PAGE_BITS = 12;
constexpr u64 PAGE_SIZE = 1ull << PAGE_BITS; // 4 KB
constexpr int LEVEL_BITS = 9;
constexpr int LEVELS = 4;
constexpr u64 ENTRIES = 1ull << LEVEL_BITS; // 512 PTEs per table
constexpr int PPN_BITS = 40;

// PTE bits (levels 1-3 use PS at bit 7, level 4 uses D at bit 6)
constexpr u64 PTE_P = 1ull << 0; // present
constexpr u64 PTE_RW = 1ull << 1; // read/write
constexpr u64 PTE_US = 1ull << 2; // user/supervisor
constexpr u64 PTE_WT = 1ull << 3; // write-through
constexpr u64 PTE_CD = 1ull << 4; // cache disable
constexpr u64 PTE_A = 1ull << 5; // accessed (reference) bit
constexpr u64 PTE_D = 1ull << 6; // dirty (level 4 only)
constexpr u64 PTE_PS = 1ull << 7; // page size (levels 2,3 only)
constexpr u64 PTE_OS_SWAPPED = 1ull << 9; // "available for OS": page lives on disk
constexpr u64 PTE_ADDR_MASK = ((1ull << PPN_BITS) - 1) << PAGE_BITS; // bits 51:12

// Simulated RAM starts at PPN_BASE (1 MB) so PPNs look like real ones.
constexpr u64 PPN_BASE = 0x100;

constexpr int TLB_SETS = 16, TLB_WAYS = 4;
constexpr int LINE = 64;
constexpr int L1_SETS = 64,  L1_WAYS = 8; // 32 KB
constexpr int L2_SETS = 512, L2_WAYS = 8; // 256 KB
constexpr int L3_SETS = 8192, L3_WAYS = 16; // 8 MB
