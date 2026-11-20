/**
 * @file        heap_allocation_test.cpp
 * @brief       Unit tests for memory heap allocation behavior
 *
 * These tests validate the BaseHeap allocation, protection, and query
 * operations based on observed runtime behavior.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace {

// Shared memory instance - expensive to create, reuse across tests
rex::memory::Memory& GetTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    bool result = memory.Initialize();
    REQUIRE(result);
    initialized = true;
  }
  return memory;
}

// Helper to cast away const for heap operations
rex::memory::BaseHeap* MutableHeap(const rex::memory::BaseHeap* heap) {
  return const_cast<rex::memory::BaseHeap*>(heap);
}

}  // namespace

// =============================================================================
// Size and Alignment Rounding Tests
// =============================================================================

TEST_CASE("Heap allocation rounds size up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));  // v00000000, 4KB pages
  REQUIRE(heap != nullptr);

  SECTION("Size smaller than page size rounds to page size") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        100,   // Requested size
        4096,  // Alignment
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);
    REQUIRE(addr != 0);

    // Query to verify actual allocated size
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);  // Rounded up from 100

    heap->Release(addr, nullptr);
  }

  SECTION("Size of 4 bytes rounds to 4096") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size exactly page size stays unchanged") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Multi-page allocation") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096 * 4, 4096,  // 4 pages
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096 * 4);

    heap->Release(addr, nullptr);
  }
}

TEST_CASE("Heap allocation rounds alignment up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Small alignment rounds to page size") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 32,  // 32-byte alignment requested
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    // Address should be page-aligned (4096), not just 32-byte aligned
    CHECK((addr % 4096) == 0);

    heap->Release(addr, nullptr);
  }

  SECTION("Large alignment is respected") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 65536,  // 64KB alignment
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

// =============================================================================
// Allocation Direction Tests
// =============================================================================

TEST_CASE("Bottom-up allocation starts after reserved first 64KB", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
      false,  // bottom-up
      &addr);
  REQUIRE(result);

  // First 64KB (0x10000) is reserved, allocations start at 0x10000 or later
  CHECK(addr >= 0x10000);

  heap->Release(addr, nullptr);
}

TEST_CASE("Top-down allocation returns high addresses", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr_bottom = 0;
  uint32_t addr_top = 0;

  // Allocate bottom-up first
  heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr_bottom);

  // Allocate top-down
  heap->Alloc(4096, 4096,
              rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
              true,  // top-down
              &addr_top);

  // Top-down should be significantly higher
  CHECK(addr_top > addr_bottom);
  // Top-down should be near top of heap (heap ends at ~0x40000000)
  CHECK(addr_top > 0x1F000000);

  heap->Release(addr_bottom, nullptr);
  heap->Release(addr_top, nullptr);
}

// =============================================================================
// AllocFixed Tests
// =============================================================================

TEST_CASE("AllocFixed allocates at exact address", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  // Choose an address that should be free
  uint32_t target = 0x20000000;

  bool result =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(result);

  // Verify allocation is at exact address
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(target, &info));
  CHECK(info.base_address == target);
  CHECK(info.state != 0);  // Should be allocated

  heap->Release(target, nullptr);
}

TEST_CASE("AllocFixed reserve-only fails on already-reserved region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t target = 0x21000000;

  // First allocation: reserve only
  bool first = heap->AllocFixed(target, 4096, 4096,
                                rex::memory::kMemoryAllocationReserve,  // Reserve only
                                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(first);

  // Second reserve at same address should FAIL
  bool second =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve,  // Reserve only - should fail
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK_FALSE(second);

  heap->Release(target, nullptr);
}

TEST_CASE("AllocFixed commit on reserved region succeeds", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t target = 0x21100000;

  // First: reserve only
  bool reserve =
      heap->AllocFixed(target, 4096, 4096, rex::memory::kMemoryAllocationReserve,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(reserve);

  // Second: commit on already-reserved region should SUCCEED
  bool commit =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationCommit,  // Commit only
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK(commit);

  heap->Release(target, nullptr);
}

// =============================================================================
// Protection Tests
// =============================================================================

TEST_CASE("Protect changes page protection and returns old value", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,  // Initial: RW
      false, &addr);
  REQUIRE(result);

  SECTION("Change to read-only") {
    uint32_t old_protect = 0;
    bool protect_result = heap->Protect(addr, 4096,
                                        rex::memory::kMemoryProtectRead,  // New: Read only
                                        &old_protect);
    REQUIRE(protect_result);

    // Old protection should have been RW (3)
    CHECK(old_protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));

    // Query to verify new protection
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.protect == rex::memory::kMemoryProtectRead);
  }

  heap->Release(addr, nullptr);
}

// =============================================================================
// QueryRegionInfo Tests
// =============================================================================

TEST_CASE("QueryRegionInfo returns correct allocation info", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result =
      heap->Alloc(4096 * 4, 4096,  // 4 pages
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Query at allocation base") {
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));

    CHECK(info.base_address == addr);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 4);
    CHECK(info.protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    CHECK(info.state != 0);  // Allocated
  }

  SECTION("Query in middle of allocation") {
    uint32_t mid = addr + 4096 * 2;  // Third page
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(mid, &info));

    // base_address is the queried address
    CHECK(info.base_address == mid);
    // allocation_size should still be full allocation
    CHECK(info.allocation_size == 4096 * 4);
    // region_size is remaining size from query point
    CHECK(info.region_size == 4096 * 2);  // 2 pages left
  }

  heap->Release(addr, nullptr);
}

// =============================================================================
// Release Tests
// =============================================================================

TEST_CASE("Release frees memory for reallocation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x22000000));  // Use a different region
  REQUIRE(heap != nullptr);

  uint32_t target = 0x22000000;

  // Allocate
  bool alloc1 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(alloc1);

  // Release
  uint32_t released_size = 0;
  bool release = heap->Release(target, &released_size);
  REQUIRE(release);
  CHECK(released_size == 4096);

  // Should be able to allocate at same address again
  bool alloc2 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK(alloc2);

  heap->Release(target, nullptr);
}

// =============================================================================
// Heap Selection Tests (LookupHeap)
// =============================================================================

TEST_CASE("LookupHeap returns correct heap for address", "[memory][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Address in v00000000 range") {
    auto* heap = memory.LookupHeap(0x10000000);
    REQUIRE(heap != nullptr);
    // This is the 4KB page heap
  }

  SECTION("Address in v40000000 range") {
    auto* heap = memory.LookupHeap(0x50000000);
    REQUIRE(heap != nullptr);
    // This is the 64KB page heap
  }

  SECTION("Address in v80000000 range (XEX)") {
    auto* heap = memory.LookupHeap(0x82000000);
    REQUIRE(heap != nullptr);
  }

  SECTION("Address in stack range returns nullptr") {
    auto* heap = memory.LookupHeap(0x7F000000);
    CHECK(heap == nullptr);
  }
}

// =============================================================================
// 64KB Page Heap Tests (v40000000)
// =============================================================================

TEST_CASE("64KB page heap rounds to 64KB boundaries", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap
  REQUIRE(heap != nullptr);

  SECTION("Small size rounds to 64KB") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 4096,  // Request only 4KB
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    // Size should be rounded to 64KB page size
    CHECK(info.allocation_size == 65536);

    // Address should be 64KB aligned
    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

// =============================================================================
// Decommit Tests
// =============================================================================

TEST_CASE("Decommit removes commit flag but keeps reservation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Verify initial state is committed
  rex::memory::HeapAllocationInfo info_before{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_before));
  CHECK((info_before.state & rex::memory::kMemoryAllocationCommit) != 0);
  CHECK((info_before.state & rex::memory::kMemoryAllocationReserve) != 0);

  // Decommit
  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  // Verify commit flag is removed but reserve remains
  rex::memory::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::memory::kMemoryAllocationCommit) == 0);
  CHECK((info_after.state & rex::memory::kMemoryAllocationReserve) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit partial region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result =
      heap->Alloc(4096 * 4, 4096,  // 4 pages
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit only middle 2 pages
  bool decommit = heap->Decommit(addr + 4096, 4096 * 2);
  REQUIRE(decommit);

  // First page should still be committed
  rex::memory::HeapAllocationInfo info_first{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_first));
  CHECK((info_first.state & rex::memory::kMemoryAllocationCommit) != 0);

  // Middle page should be decommitted
  rex::memory::HeapAllocationInfo info_mid{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096, &info_mid));
  CHECK((info_mid.state & rex::memory::kMemoryAllocationCommit) == 0);

  // Last page should still be committed
  rex::memory::HeapAllocationInfo info_last{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096 * 3, &info_last));
  CHECK((info_last.state & rex::memory::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit-recommit cycle on 64KB heap (real usage pattern)", "[memory][heap]") {
  // This pattern observed in real app: NtFreeVirtualMemory(MEM_DECOMMIT)
  // followed by NtAllocateVirtualMemory at same address
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap, 64KB pages
  REQUIRE(heap != nullptr);

  // Allocate on 64KB heap
  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit (like NtFreeVirtualMemory with type=0x4000)
  bool decommit = heap->Decommit(addr, 65536);
  REQUIRE(decommit);

  // After decommit, state should be Reserve only (state=1)
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK(info.state == rex::memory::kMemoryAllocationReserve);  // Exactly 1

  // Recommit at same address (like NtAllocateVirtualMemory with type=0x60001000)
  bool recommit =
      heap->AllocFixed(addr, 65536, 65536,
                       rex::memory::kMemoryAllocationCommit,  // Commit only
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(recommit);

  // State should now include commit again
  rex::memory::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::memory::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Repeated decommit of same page succeeds (idempotent)", "[memory][heap]") {
  // Real app decommits same address multiple times in sequence
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // First decommit
  bool decommit1 = heap->Decommit(addr, 65536);
  REQUIRE(decommit1);

  // Second decommit of same already-decommitted page should still succeed
  bool decommit2 = heap->Decommit(addr, 65536);
  CHECK(decommit2);

  // Third decommit
  bool decommit3 = heap->Decommit(addr, 65536);
  CHECK(decommit3);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit on 64KB heap uses 64KB granularity", "[memory][heap]") {
  // On v40000000 heap, even small decommit requests affect whole 64KB page
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit with small size - should still affect the page
  bool decommit = heap->Decommit(addr, 4096);  // Request only 4KB
  REQUIRE(decommit);

  // The 64KB page should still be affected (implementation detail:
  // size gets rounded to page_size internally via get_page_count)
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  // State should show decommitted
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) == 0);

  heap->Release(addr, nullptr);
}

// =============================================================================
// Address Translation Tests
// =============================================================================

TEST_CASE("TranslateVirtual returns valid host pointer", "[memory][translation]") {
  auto& memory = GetTestMemory();

  // Allocate some memory first
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t guest_addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest_addr);
  REQUIRE(result);

  // Translate to host
  uint8_t* host_ptr = memory.TranslateVirtual(guest_addr);
  REQUIRE(host_ptr != nullptr);

  // Should be able to read/write
  host_ptr[0] = 0xAB;
  CHECK(host_ptr[0] == 0xAB);

  heap->Release(guest_addr, nullptr);
}

TEST_CASE("HostToGuestVirtual roundtrip", "[memory][translation]") {
  auto& memory = GetTestMemory();

  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t original_guest = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &original_guest);
  REQUIRE(result);

  // Guest -> Host -> Guest roundtrip
  uint8_t* host_ptr = memory.TranslateVirtual(original_guest);
  uint32_t back_to_guest = memory.HostToGuestVirtual(host_ptr);

  CHECK(back_to_guest == original_guest);

  heap->Release(original_guest, nullptr);
}

TEST_CASE("TranslatePhysical masks to 29 bits", "[memory][translation]") {
  auto& memory = GetTestMemory();

  // Physical addresses are masked with 0x1FFFFFFF (29 bits)
  // So 0xA0000000 and 0x00000000 should map to same physical offset
  uint8_t* ptr_low = memory.TranslatePhysical(0x00001000);
  uint8_t* ptr_high = memory.TranslatePhysical(0xA0001000);

  // Both should resolve to same offset in physical memory
  // (0xA0001000 & 0x1FFFFFFF) == 0x00001000
  CHECK(ptr_low == ptr_high);
}

// =============================================================================
// Physical Heap Tests
// =============================================================================

TEST_CASE("Physical heap vA0000000 (64KB pages, cached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xA0000000));
  REQUIRE(heap != nullptr);

  // vA0000000 heap: base=0xA0000000, 64KB pages
  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xA0000000-0xBFFFFFFF range
  CHECK(addr >= 0xA0000000);
  CHECK(addr < 0xC0000000);

  // Should be 64KB aligned
  CHECK((addr % 65536) == 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Physical heap vC0000000 (16MB pages, uncached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xC0000000));
  REQUIRE(heap != nullptr);

  // vC0000000 heap: base=0xC0000000, 16MB pages
  // Note: First 16MB is pre-allocated for GPU writeback

  uint32_t addr = 0;
  bool result =
      heap->Alloc(16 * 1024 * 1024, 16 * 1024 * 1024,  // 16MB
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xC0000000-0xDFFFFFFF range
  CHECK(addr >= 0xC0000000);
  CHECK(addr < 0xE0000000);

  heap->Release(addr, nullptr);
}

TEST_CASE("Physical heap vE0000000 (4KB pages, write-combine)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xE0000000));
  REQUIRE(heap != nullptr);

  // vE0000000 heap: base=0xE0000000, 4KB pages
  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xE0000000-0xFFCFFFFF range
  CHECK(addr >= 0xE0000000);
  CHECK(addr < 0xFFD00000);

  // Should be 4KB aligned
  CHECK((addr % 4096) == 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("LookupHeapByType selects correct heap", "[memory][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Virtual heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(false, 4096);
    REQUIRE(heap != nullptr);
    // Should be v00000000 heap
  }

  SECTION("Virtual heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(false, 65536);
    REQUIRE(heap != nullptr);
    // Should be v40000000 heap
  }

  SECTION("Physical heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(true, 4096);
    REQUIRE(heap != nullptr);
    // Should be vE0000000 heap
  }

  SECTION("Physical heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(true, 65536);
    REQUIRE(heap != nullptr);
    // Should be vA0000000 heap
  }

  SECTION("Physical heap with 16MB pages") {
    auto* heap = memory.LookupHeapByType(true, 16 * 1024 * 1024);
    REQUIRE(heap != nullptr);
    // Should be vC0000000 heap
  }
}

// =============================================================================
// Table-only guest protection changes on physical memory
// (cvars cheap_physical_protect and physical_protect_timing)
// =============================================================================

// Both cvars are defined in rexruntime.dll. The test exe links its own copy of
// the cvar registry (rexcore is an OBJECT library), so setting them by name
// would reach the wrong registry: they are set through the DLL's storage
// accessors. Test 02's table-only result shows the switch reached the DLL.
REXCVAR_DECLARE(bool, cheap_physical_protect);
REXCVAR_DECLARE(bool, physical_protect_timing);

namespace {

using rex::memory::PageAccess;
using rex::memory::PhysicalHeap;
using rex::memory::PhysicalProtectStats;

// The guest's read/write value (MmSetAddressProtect 4).
constexpr uint32_t kRW = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
// Plain read only: the one value that makes the host read only on today's path.
constexpr uint32_t kR = rex::memory::kMemoryProtectRead;
// The game's read-only value (MmSetAddressProtect 1026): host read/write today.
constexpr uint32_t kRWC = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWriteCombine;
constexpr uint32_t kReserveCommit =
    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
// Page size of the 0xA0000000 view.
constexpr uint32_t kViewPage = 0x10000;

// Sets both switches; turns both off when the test ends, also on a failed
// REQUIRE.
class Switches {
 public:
  Switches(bool cheap, bool timing) { Set(cheap, timing); }
  ~Switches() { Set(false, false); }
  Switches(const Switches&) = delete;
  Switches& operator=(const Switches&) = delete;
  static void Set(bool cheap, bool timing) {
    REXCVAR_SET(cheap_physical_protect, cheap);
    REXCVAR_SET(physical_protect_timing, timing);
  }
};

PhysicalHeap* ViewHeap(rex::memory::Memory& memory) {
  return static_cast<PhysicalHeap*>(MutableHeap(memory.LookupHeap(0xA0000000)));
}

PageAccess HostAccess(void* host_address) {
  size_t length = 1;
  PageAccess access = PageAccess::kExecuteReadWrite;
  REQUIRE(rex::memory::QueryProtect(host_address, length, access));
  return access;
}

PageAccess ViewHost(rex::memory::Memory& memory, uint32_t address) {
  return HostAccess(memory.TranslateVirtual(address));
}

PageAccess ParentHost(rex::memory::Memory& memory, PhysicalHeap* heap, uint32_t address) {
  return HostAccess(memory.TranslatePhysical(heap->GetPhysicalAddress(address)));
}

uint32_t TableProtect(rex::memory::BaseHeap* heap, uint32_t address) {
  uint32_t protect = 0xFFFFFFFFu;
  REQUIRE(heap->QueryProtect(address, &protect));
  return protect;
}

uint32_t ParentTableProtect(rex::memory::Memory& memory, PhysicalHeap* heap, uint32_t address) {
  return TableProtect(memory.GetPhysicalHeap(), heap->GetPhysicalAddress(address));
}

bool Marked(rex::memory::BaseHeap* heap, uint32_t address) {
  bool marked = false;
  REQUIRE(heap->QueryHostKeptWritable(address, &marked));
  return marked;
}

bool ParentMarked(rex::memory::Memory& memory, PhysicalHeap* heap, uint32_t address) {
  return Marked(memory.GetPhysicalHeap(), heap->GetPhysicalAddress(address));
}

PhysicalProtectStats StatsDelta(const PhysicalProtectStats& before,
                                const PhysicalProtectStats& after) {
  return PhysicalProtectStats{after.calls - before.calls,
                              after.table_only_calls - before.table_only_calls,
                              after.host_calls - before.host_calls,
                              after.total_ns - before.total_ns, after.host_ns - before.host_ns};
}

// One allocation on the 0xA0000000 view, released by the test (REQUIRE on the
// result) or, after a failed REQUIRE, at scope exit.
class ViewAlloc {
 public:
  ViewAlloc(PhysicalHeap* heap, uint32_t size, uint32_t protect, uint32_t alignment = kViewPage)
      : heap_(heap) {
    REQUIRE(heap_->Alloc(size, alignment, kReserveCommit, protect, true, &address_));
    held_ = true;
  }
  ~ViewAlloc() {
    if (held_) {
      heap_->Release(address_);
    }
  }
  ViewAlloc(const ViewAlloc&) = delete;
  ViewAlloc& operator=(const ViewAlloc&) = delete;
  uint32_t address() const { return address_; }
  void Release() {
    held_ = false;
    REQUIRE(heap_->Release(address_));
  }

 private:
  PhysicalHeap* heap_;
  uint32_t address_ = 0;
  bool held_ = false;
};

// Fixture precondition after a read/write allocation: table, both host views
// read/write, no mark in either table.
void RequireFreshReadWrite(rex::memory::Memory& memory, PhysicalHeap* heap, uint32_t address) {
  REQUIRE(TableProtect(heap, address) == kRW);
  REQUIRE(ParentTableProtect(memory, heap, address) == kRW);
  REQUIRE(ViewHost(memory, address) == PageAccess::kReadWrite);
  REQUIRE(ParentHost(memory, heap, address) == PageAccess::kReadWrite);
  REQUIRE_FALSE(Marked(heap, address));
  REQUIRE_FALSE(ParentMarked(memory, heap, address));
}

// Counts invalidation callbacks while registered; unregisters at scope exit.
class CallbackCounter {
 public:
  explicit CallbackCounter(rex::memory::Memory& memory) : memory_(memory) {
    handle_ = memory_.RegisterPhysicalMemoryInvalidationCallback(&Callback, this);
  }
  ~CallbackCounter() { memory_.UnregisterPhysicalMemoryInvalidationCallback(handle_); }
  CallbackCounter(const CallbackCounter&) = delete;
  CallbackCounter& operator=(const CallbackCounter&) = delete;
  int count() const { return count_; }

 private:
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t, uint32_t, bool) {
    ++static_cast<CallbackCounter*>(context)->count_;
    return {0, UINT32_MAX};
  }
  rex::memory::Memory& memory_;
  void* handle_ = nullptr;
  int count_ = 0;
};

}  // namespace

TEST_CASE("physprotect 01: switch off, RW to R takes the full path", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(false, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);
  CHECK(d.host_ns > 0);
  CHECK(d.total_ns >= d.host_ns);

  alloc.Release();
}

TEST_CASE("physprotect 02: switch on, RW to R is table-only and marks", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK(Marked(heap, a));
  CHECK(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.host_calls == 0);
  CHECK(d.table_only_calls == 1);

  alloc.Release();
}

TEST_CASE("physprotect 03: switch on, marked R to RW is table-only and clears the mark",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.host_calls == 0);
  CHECK(d.table_only_calls == 1);

  alloc.Release();
}

TEST_CASE("physprotect 04: switch on, page allocated R to RW takes the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kR);
  const uint32_t a = alloc.address();
  REQUIRE(TableProtect(heap, a) == kR);
  REQUIRE_FALSE(Marked(heap, a));

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 05: switch on, a watched RW page takes the full path both ways",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  CallbackCounter callbacks(memory);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  memory.EnablePhysicalMemoryAccessCallbacks(heap->GetPhysicalAddress(a), kViewPage, true, false);
  // Control: the watch made the view host read only.
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(callbacks.count() == 0);
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(callbacks.count() == 1);
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 06: marked page to no access takes the full path and clears the mark",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(Marked(heap, a));
  CHECK(d.table_only_calls == 1);
  CHECK(d.host_calls == 0);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, rex::memory::kMemoryProtectNoAccess));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == rex::memory::kMemoryProtectNoAccess);
  CHECK(ViewHost(memory, a) == PageAccess::kNoAccess);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kNoAccess);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 07: old_protect requested takes the full path", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  uint32_t old_protect = 0xFFFFFFFFu;
  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR, &old_protect));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(old_protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 08: release and reallocate leaves no mark", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc first(heap, kViewPage, kRW);
  const uint32_t a = first.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  CHECK(Marked(heap, a));
  first.Release();

  ViewAlloc second(heap, kViewPage, kR);
  // Without the same address the test proves nothing.
  CHECK(second.address() == a);
  const uint32_t b = second.address();
  CHECK_FALSE(Marked(heap, b));
  CHECK_FALSE(ParentMarked(memory, heap, b));

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(b, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, b) == kRW);
  CHECK(ViewHost(memory, b) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, b) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, b));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  second.Release();
}

TEST_CASE("physprotect 09: a range with one unmarked read-only page takes the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, 2 * kViewPage, kR);
  const uint32_t a = alloc.address();
  const uint32_t b = a + kViewPage;
  REQUIRE(TableProtect(heap, a) == kR);
  REQUIRE(TableProtect(heap, b) == kR);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(d.table_only_calls == 1);
  CHECK(d.host_calls == 0);
  CHECK(Marked(heap, a));
  CHECK_FALSE(Marked(heap, b));

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, 2 * kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(TableProtect(heap, b) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ViewHost(memory, b) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, b) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(Marked(heap, b));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, b));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 10: the table-only path writes the parent table", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(ParentTableProtect(memory, heap, a) == kR);
  CHECK(d.table_only_calls == 1);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(ParentTableProtect(memory, heap, a) == kRW);
  CHECK(d.table_only_calls == 1);

  alloc.Release();
}

TEST_CASE("physprotect 11: a full-path change through the C0 alias clears the parent's mark",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  const uint32_t phys = heap->GetPhysicalAddress(a);
  const uint32_t alias = 0xC0000000u + phys;
  auto* alias_heap = static_cast<PhysicalHeap*>(MutableHeap(memory.LookupHeap(alias)));
  REQUIRE(alias_heap != nullptr);
  REQUIRE(alias_heap->heap_base() == 0xC0000000u);
  REQUIRE(alias_heap->GetPhysicalAddress(alias) == phys);

  REQUIRE(heap->Protect(a, kViewPage, kR));
  CHECK(Marked(heap, a));
  CHECK(ParentMarked(memory, heap, a));

  // The alias page is uncommitted in the C0 view: the parent's full Protect
  // runs, then the view's fails.
  const bool alias_result = alias_heap->Protect(alias, kViewPage, kR);
  CHECK_FALSE(alias_result);
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(ParentTableProtect(memory, heap, a) == kR);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ParentTableProtect(memory, heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 12: with the switch off a marked page takes the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  CHECK(Marked(heap, a));

  Switches::Set(false, true);
  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 13: PageEntry is 16 bytes and the mark is qword bit 62",
          "[memory][physprotect]") {
  Switches switches(false, false);
  static_assert(sizeof(rex::memory::PageEntry) == 16,
                "PageEntry: a uint64_t and a 12-byte struct, rounded up to 8-byte alignment");
  rex::memory::PageEntry entry{};
  entry.host_kept_writable = 1;
  CHECK(entry.qword == (uint64_t(1) << 62));
}

TEST_CASE("physprotect 14: with the timing cvar off nothing is counted", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, false);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(Marked(heap, a));
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(d.calls == 0);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 0);
  CHECK(d.total_ns == 0);
  CHECK(d.host_ns == 0);

  alloc.Release();
}

TEST_CASE("physprotect 15: a watch on a marked page sends the change to RW down the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  CallbackCounter callbacks(memory);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRWC));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRWC);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(Marked(heap, a));
  CHECK(d.table_only_calls == 1);
  CHECK(d.host_calls == 0);

  memory.EnablePhysicalMemoryAccessCallbacks(heap->GetPhysicalAddress(a), kViewPage, true, false);
  // Control: the watch made the view host read only (RWC counts as writable
  // for the watch).
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(callbacks.count() == 0);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(callbacks.count() == 1);
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 16: the game's pair RW to RWC to RW is table-only both ways",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRWC));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRWC);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK(Marked(heap, a));
  CHECK(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.host_calls == 0);
  CHECK(d.table_only_calls == 1);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.host_calls == 0);
  CHECK(d.table_only_calls == 1);

  alloc.Release();
}

TEST_CASE("physprotect 17: read with NoCache takes the full path", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  const uint32_t read_nocache = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectNoCache;
  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, read_nocache));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == read_nocache);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 18: no access to R takes the full path", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, rex::memory::kMemoryProtectNoAccess));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 19: decommit and recommit leave no mark", "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  CHECK(Marked(heap, a));

  REQUIRE(heap->Decommit(a, kViewPage));
  REQUIRE(heap->AllocFixed(a, kViewPage, kViewPage, rex::memory::kMemoryAllocationCommit, kR));
  CHECK(TableProtect(heap, a) == kR);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

// Fix round 1 (review task-1-review.md: I1, M1, M3, M4, M5). Expected values
// come from the review and from today's full path (ToPageAccess of the new
// value applied to both host mappings).

TEST_CASE("physprotect 20: unmarked plain R to RWC takes the full path (host access rises)",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kR);
  const uint32_t a = alloc.address();
  REQUIRE(TableProtect(heap, a) == kR);
  REQUIRE_FALSE(Marked(heap, a));
  // Fixture: plain read only is read only in both host mappings.
  REQUIRE(ViewHost(memory, a) == PageAccess::kReadOnly);
  REQUIRE(ParentHost(memory, heap, a) == PageAccess::kReadOnly);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRWC));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRWC);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 21: unmarked R to R is table-only and leaves no mark",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kR);
  const uint32_t a = alloc.address();
  REQUIRE(TableProtect(heap, a) == kR);
  REQUIRE_FALSE(Marked(heap, a));
  REQUIRE(ViewHost(memory, a) == PageAccess::kReadOnly);
  REQUIRE(ParentHost(memory, heap, a) == PageAccess::kReadOnly);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 1);
  CHECK(d.host_calls == 0);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 22: a Write value outside class read/write takes the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  REQUIRE(Marked(heap, a));

  // Write without read: ToPageAccess gives no access on today's path.
  const uint32_t write_only = rex::memory::kMemoryProtectWrite;
  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, write_only));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == write_only);
  CHECK(ViewHost(memory, a) == PageAccess::kNoAccess);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kNoAccess);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 23: old_protect requested on a change to RW takes the full path",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  REQUIRE(Marked(heap, a));

  uint32_t old_protect = 0xFFFFFFFFu;
  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW, &old_protect));
  auto d = StatsDelta(before, heap->protect_stats());
  // The full path wrote the out value. Which value it should be after a
  // table-only change is open (fix round 1): today's base gives
  // kMemoryProtectRead; this code reads the parent's host, which the
  // table-only change left read/write, and gives Read|Write.
  CHECK(old_protect != 0xFFFFFFFFu);
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 24: a watch elsewhere in the guest page keeps a sub-page R change full",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  // Watch only the second 4 KiB system page of the 64 KiB guest page.
  memory.EnablePhysicalMemoryAccessCallbacks(heap->GetPhysicalAddress(a) + 0x1000, 0x1000, true,
                                             false);
  // Control: only that system page became read only.
  CHECK(ViewHost(memory, a + 0x1000) == PageAccess::kReadOnly);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, 0x1000, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 25: a watched page on the E0 view takes the full path both ways",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  auto* heap = static_cast<PhysicalHeap*>(MutableHeap(memory.LookupHeap(0xE0000000)));
  REQUIRE(heap != nullptr);
  REQUIRE(heap->heap_base() == 0xE0000000u);
  // The case under test: the E0 view's host mapping is offset by 0x1000.
  REQUIRE(heap->host_address_offset() == 0x1000u);
  CallbackCounter callbacks(memory);
  ViewAlloc alloc(heap, 0x1000, kRW, 0x1000);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  memory.EnablePhysicalMemoryAccessCallbacks(heap->GetPhysicalAddress(a), 0x1000, true, false);
  // Control: the watch made the view host read only.
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, 0x1000, kR));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kR);
  CHECK(ViewHost(memory, a) == PageAccess::kReadOnly);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadOnly);
  CHECK_FALSE(Marked(heap, a));
  CHECK(callbacks.count() == 0);
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  before = heap->protect_stats();
  REQUIRE(heap->Protect(a, 0x1000, kRW));
  d = StatsDelta(before, heap->protect_stats());
  CHECK(callbacks.count() == 1);
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}

TEST_CASE("physprotect 26: AllocFixed commit over a committed marked page clears the mark",
          "[memory][physprotect]") {
  auto& memory = GetTestMemory();
  Switches switches(true, true);
  PhysicalHeap* heap = ViewHeap(memory);
  REQUIRE(heap != nullptr);
  ViewAlloc alloc(heap, kViewPage, kRW);
  const uint32_t a = alloc.address();
  RequireFreshReadWrite(memory, heap, a);
  REQUIRE(heap->Protect(a, kViewPage, kR));
  REQUIRE(Marked(heap, a));

  REQUIRE(heap->AllocFixed(a, kViewPage, kViewPage, rex::memory::kMemoryAllocationCommit, kR));
  CHECK(TableProtect(heap, a) == kR);
  CHECK_FALSE(Marked(heap, a));
  CHECK_FALSE(ParentMarked(memory, heap, a));

  auto before = heap->protect_stats();
  REQUIRE(heap->Protect(a, kViewPage, kRW));
  auto d = StatsDelta(before, heap->protect_stats());
  CHECK(TableProtect(heap, a) == kRW);
  CHECK(ViewHost(memory, a) == PageAccess::kReadWrite);
  CHECK(ParentHost(memory, heap, a) == PageAccess::kReadWrite);
  CHECK_FALSE(Marked(heap, a));
  CHECK(d.calls == 1);
  CHECK(d.table_only_calls == 0);
  CHECK(d.host_calls == 2);

  alloc.Release();
}
