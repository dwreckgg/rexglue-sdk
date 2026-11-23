/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/submit_snapshot.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

// TEMP DIAGNOSTIC (N2 probe); the DOA4_GX seam word n2stretch sets it to 2.
// Bounded to 0..10: the wait spins with the global lock held at every frame
// end, and the cvar is live in the settings overlay.
REXCVAR_DEFINE_INT32(gpu_refresh_stretch_ms, 0, "GPU",
                     "Test switch that widens the frame-end valid-flag refresh window by waiting "
                     "this many milliseconds between its copy and its pointer exchange; 0 = off.")
    .range(0, 10);

// From lmaogoodcodenotreally/rexglue-sdk@59e13bba, where it is on by default;
// off by default here (the gx trial sets it through DOA4_GX). The game's Esc menu
// offers it as "Batch GPU uploads"; the description stays short so the settings
// overlay's unwrapped tooltip shows it whole. Which pages are uploaded: with
// clear_memory_page_state on (the default) every page the frame-end reset cleared,
// written or not, and a CPU write to it before its draw is then caught only by the
// write watch; with it off, only the pages the CPU has written again since.
REXCVAR_DEFINE_BOOL(gpu_upload_prefetch, false, "GPU",
                    "Upload in one batch at each frame's end, rather than draw by draw, the guest "
                    "memory pages draws read two frames ago that are invalid then. Less GPU work a "
                    "frame; moving parts such as hair can be drawn one frame late now and then. "
                    "Off by default.");

// Submit snapshot (include/rex/graphics/submit_snapshot.h): read at each VdSwap
// (xboxkrnl_video.cpp) and latched by the GPU thread at each swap packet.
// Descriptions short: the settings overlay's tooltip is unwrapped.
REXCVAR_DEFINE_BOOL(gpu_submit_snapshot, true, "GPU",
                    "Draw animated models from a copy of their vertex data taken at the guest's "
                    "swap, so no model is drawn half from the next frame (seams, jumps). On by "
                    "default.");
REXCVAR_DEFINE_BOOL(gpu_submit_snapshot_check, false, "GPU",
                    "Test switch: counts ranges that moved around the VdSwap copy (submit "
                    "snapshot stats line). Off by default.");

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  valid_buffer_a_.assign(num_system_page_flags_, 0);
  valid_buffer_b_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  active_valid_flags_.store(valid_buffer_a_.data(), std::memory_order_relaxed);
  staging_valid_flags_.store(valid_buffer_b_.data(), std::memory_order_relaxed);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  ReleaseTraceDownloadRanges();

  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  active_valid_flags_.store(nullptr, std::memory_order_relaxed);
  staging_valid_flags_.store(nullptr, std::memory_order_relaxed);
  valid_buffer_a_.clear();
  valid_buffer_a_.shrink_to_fit();
  valid_buffer_b_.clear();
  valid_buffer_b_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  uint64_t* active = active_valid_flags_.load(std::memory_order_relaxed);
  uint64_t* staging = staging_valid_flags_.load(std::memory_order_relaxed);
  if (active && num_system_page_flags_) {
    std::memset(active, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (staging && num_system_page_flags_) {
    std::memset(staging, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (!system_page_flags_valid_and_gpu_written_.empty()) {
    std::memset(system_page_flags_valid_and_gpu_written_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
  }

  // Force a refresh on the next frame-end sync.
  dirty_blocks_.store(UINT32_MAX, std::memory_order_relaxed);
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  // The whole refresh holds the global lock, as the invalidation callback does
  // (official ReXGlue resets these flags under its lock too). Without it a
  // guest write fault landing between the dirty-mask exchange and the pointer
  // exchange cleared its page in the buffer about to become staging (or in a
  // block left out of a partial copy), so the new active buffer kept a stale
  // valid bit and the GPU kept the stale page; and the dirty-flag reset below
  // erased that fault's dirty mark. A guest fault now waits at most one
  // refresh body (a copy of at most num_system_page_flags_ words).
  auto global_lock = global_critical_region_.Acquire();

  // TEMP DIAGNOSTIC (N2 probe): lock_us_max. The hold is timed from here (the
  // Acquire's own wait is not in it) to the end of the function, the early
  // returns included; the stretch's measured duration is subtracted. Declared
  // after global_lock, so it is destroyed (and records) while the lock is held.
  struct RefreshLockHoldTimer {
    SharedMemory& shared_memory;
    const uint64_t start_ticks;
    uint64_t excluded_ticks;
    ~RefreshLockHoldTimer() {
      const uint64_t held_ticks = rex::chrono::Clock::QueryHostTickCount() - start_ticks;
      shared_memory.NoteRefreshLockHold(held_ticks > excluded_ticks ? held_ticks - excluded_ticks
                                                                    : 0);
    }
  } lock_hold_timer{*this, rex::chrono::Clock::QueryHostTickCount(), 0};

  if (!gpu_written_data_dirty_.load(std::memory_order_relaxed)) {
    return;
  }

  uint64_t* staging = staging_valid_flags_.load(std::memory_order_acquire);
  if (!staging || !num_system_page_flags_) {
    gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
    dirty_blocks_.store(0, std::memory_order_relaxed);
    return;
  }

  uint32_t dirty_mask = dirty_blocks_.exchange(0, std::memory_order_relaxed);
  uint32_t dirty_count = rex::bit_count(dirty_mask);

  // TEMP DIAGNOSTIC (N2 probe): open the window (odd) right before the copy.
  // Entries stored late by a callback of the previous window are wiped here.
  for (std::atomic<uint32_t>& entry : refresh_pages_) {
    entry.store(kNoRefreshPage, std::memory_order_relaxed);
  }
  refresh_page_count_.store(0, std::memory_order_relaxed);
  refresh_in_window_.fetch_add(1, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (dirty_count == 0 || dirty_count > 16) {
    std::memcpy(staging, system_page_flags_valid_and_gpu_written_.data(),
                num_system_page_flags_ * sizeof(uint64_t));
  } else {
    while (dirty_mask) {
      uint32_t block_index;
      rex::bit_scan_forward(dirty_mask, &block_index);
      dirty_mask &= ~(uint32_t(1) << block_index);
      uint32_t entry_offset = block_index * 64;
      if (entry_offset >= num_system_page_flags_) {
        continue;
      }
      uint32_t entry_count = std::min(uint32_t(64), num_system_page_flags_ - entry_offset);
      std::memcpy(staging + entry_offset,
                  system_page_flags_valid_and_gpu_written_.data() + entry_offset,
                  entry_count * sizeof(uint64_t));
    }
  }

  // TEMP DIAGNOSTIC (N2 probe): the test switch that widens the window between
  // the copy and the exchange. A yielding wait on the steady clock, not a
  // sleep: a Windows sleep of 2 ms can last a whole 15.6 ms timer tick.
  // The wait now sits inside the global lock, so a guest fault that arrives
  // during it waits instead of landing in the window (test switch only).
  const int32_t stretch_ms = REXCVAR_GET(gpu_refresh_stretch_ms);
  if (stretch_ms > 0) {
    const uint64_t stretch_start_ticks = rex::chrono::Clock::QueryHostTickCount();
    const auto stretch_end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(stretch_ms);
    while (std::chrono::steady_clock::now() < stretch_end) {
      std::this_thread::yield();
    }
    refresh_stretches_.fetch_add(1, std::memory_order_relaxed);
    lock_hold_timer.excluded_ticks =
        rex::chrono::Clock::QueryHostTickCount() - stretch_start_ticks;
  }

  uint64_t* old_active = active_valid_flags_.exchange(staging, std::memory_order_acq_rel);
  staging_valid_flags_.store(old_active, std::memory_order_release);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);

  // TEMP DIAGNOSTIC (N2 probe): close the window (even), then read the count,
  // walk the stored pages and reset. A stored page is lost if the new active
  // buffer (staging, copied before the callback cleared the page) still says
  // valid while the GPU-written flags say not valid. Read under the lock.
  refresh_in_window_.fetch_add(1, std::memory_order_seq_cst);
  const uint32_t stored_count = refresh_page_count_.load(std::memory_order_acquire);
  const uint32_t walk_count = std::min(stored_count, kRefreshProbePages);
  uint64_t lost = 0;
  for (uint32_t i = 0; i < walk_count; ++i) {
    const uint32_t page = refresh_pages_[i].load(std::memory_order_acquire);
    if (page == kNoRefreshPage || (page >> 6) >= num_system_page_flags_) {
      continue;  // counted by the callback but not stored yet: dropped
    }
    const uint64_t page_bit = uint64_t(1) << (page & 63);
    if ((staging[page >> 6] & page_bit) &&
        !(system_page_flags_valid_and_gpu_written_[page >> 6] & page_bit)) {
      ++lost;
    }
  }
  if (lost) {
    refresh_lost_.fetch_add(lost, std::memory_order_relaxed);
  }
  refresh_page_count_.store(0, std::memory_order_relaxed);
}

SharedMemory::RefreshProbe SharedMemory::TakeRefreshProbe() {
  RefreshProbe probe;
  probe.window_hits = refresh_window_hits_.exchange(0, std::memory_order_relaxed);
  probe.lost = refresh_lost_.exchange(0, std::memory_order_relaxed);
  probe.stretches = refresh_stretches_.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_ticks_max = refresh_lock_ticks_max_.exchange(0, std::memory_order_relaxed);
  const uint64_t tick_frequency = rex::chrono::Clock::QueryHostTickFrequency();
  probe.lock_us_max =
      tick_frequency ? double(lock_ticks_max) * 1.0e6 / double(tick_frequency) : 0.0;
  return probe;
}

void SharedMemory::NoteRefreshLockHold(uint64_t ticks) {
  uint64_t current_max = refresh_lock_ticks_max_.load(std::memory_order_relaxed);
  while (ticks > current_max &&
         !refresh_lock_ticks_max_.compare_exchange_weak(current_max, ticks,
                                                        std::memory_order_relaxed)) {
  }
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();
    uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      if (valid_flags) {
        valid_flags[i] |= valid_bits;
      }
      uint64_t old_gpu_written = system_page_flags_valid_and_gpu_written_[i];
      uint64_t new_gpu_written =
          written_by_gpu ? (old_gpu_written | valid_bits) : (old_gpu_written & ~valid_bits);
      if (new_gpu_written != old_gpu_written) {
        system_page_flags_valid_and_gpu_written_[i] = new_gpu_written;
        gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
        dirty_blocks_.fetch_or(uint32_t(1) << (i >> 6), std::memory_order_relaxed);
      }
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                                 uint32_t* uploaded_pages) {
  if (uploaded_pages) {
    *uploaded_pages = 0;
  }
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
    NoteRequested(range.first, range.second);
  }

  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_acquire);
  if (valid_flags) {
    bool all_valid = true;
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      if (!range.second) {
        continue;
      }
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags[i];
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }
        if (block_valid != UINT64_MAX) {
          all_valid = false;
          break;
        }
      }
      if (!all_valid) {
        break;
      }
    }
    if (all_valid) {
      COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
      COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                        uint32_t(merged_ranges.size()));
      COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count", 0);
      return true;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags ? valid_flags[i] : 0;
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  if (uploaded_pages) {
    uint32_t pages = 0;
    for (const std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
      pages += upload_range.second;
    }
    *uploaded_pages = pages;
  }
  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length, uint32_t* uploaded_pages) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1, uploaded_pages);
}

bool SharedMemory::UploadPrefetchEnabled() const { return REXCVAR_GET(gpu_upload_prefetch); }

void SharedMemory::NoteRequested(uint32_t start, uint32_t length) {
  std::vector<uint64_t>& cur = upload_history_[0];
  if (cur.empty() || !length) {
    return;
  }
  const uint32_t first = start >> page_size_log2_;
  const uint32_t last = (start + (length - 1)) >> page_size_log2_;
  for (uint32_t w = first >> 6; w <= last >> 6; ++w) {
    const uint32_t lo = w == first >> 6 ? first & 63 : 0;
    const uint32_t hi = w == last >> 6 ? last & 63 : 63;
    cur[w] |= (~uint64_t(0) >> (63 - (hi - lo))) << lo;
  }
}

uint32_t SharedMemory::PrefetchFrameUploads() {
  if (!UploadPrefetchEnabled()) {
    return 0;
  }
  const size_t words = num_system_page_flags_;
  if (!words) {
    return 0;
  }
  if (upload_history_[0].size() != words) {
    for (auto& history : upload_history_) {
      history.assign(words, 0);
    }
    return 0;
  }
  upload_history_[2].swap(upload_history_[1]);
  upload_history_[1].swap(upload_history_[0]);
  std::fill(upload_history_[0].begin(), upload_history_[0].end(), 0);
  upload_ranges_.clear();
  {
    auto global_lock = global_critical_region_.Acquire();
    // Adapted: the valid flags here are double-buffered; the active copy is
    // the one RequestRanges, MakeRangeValid and the invalidation callback use
    // under this lock (the copies are swapped at a frame's close, by
    // SetSystemPageBlocksValidWithGpuDataWritten on this same thread).
    const uint64_t* valid = active_valid_flags_.load(std::memory_order_relaxed);
    if (!valid) {
      return 0;
    }
    for (size_t i = 0; i < words; ++i) {
      // Only invalid pages: a valid page between them may hold data the GPU
      // wrote (a resolve), which a copy from CPU memory would undo. Invalid
      // means the write watch caught a CPU write, or, with
      // clear_memory_page_state on, the frame-end refresh just reset the page
      // (written or not); see PrefetchFrameUploads in the header.
      uint64_t bits = upload_history_[2][i] & ~valid[i];
      while (bits) {
        const uint32_t bit = uint32_t(std::countr_zero(bits));
        const uint32_t run = uint32_t(std::countr_one(bits >> bit));
        const uint32_t page = uint32_t(i) * 64 + bit;
        if (!upload_ranges_.empty() &&
            upload_ranges_.back().first + upload_ranges_.back().second == page) {
          upload_ranges_.back().second += run;
        } else {
          upload_ranges_.emplace_back(page, run);
        }
        bits &= run == 64 ? 0 : ~(((uint64_t(1) << run) - 1) << bit);
      }
    }
  }
  // Submit snapshot (latched on): no page of a learned range is uploaded here;
  // the next frame's VdSwap copy does not exist yet, so such a page stays
  // invalid and its draw-time upload splices that copy in. Both lists are
  // sorted by page and disjoint.
  submit_snapshot::State& snapshot = submit_snapshot::Global();
  if (snapshot.gpu_on() && !upload_ranges_.empty()) {
    const std::vector<std::pair<uint32_t, uint32_t>>& learned =
        snapshot.LearnedPageSpans(page_size_log2_);
    if (!learned.empty()) {
      submit_snapshot_scratch_.clear();
      size_t j = 0;
      for (const auto& [first, count] : upload_ranges_) {
        uint32_t lo = first;
        const uint32_t hi = first + count;
        while (j < learned.size() && learned[j].first + learned[j].second <= lo) {
          ++j;
        }
        for (size_t k = j; k < learned.size() && learned[k].first < hi && lo < hi; ++k) {
          if (learned[k].first > lo) {
            submit_snapshot_scratch_.emplace_back(lo, learned[k].first - lo);
          }
          lo = std::max(lo, learned[k].first + learned[k].second);
        }
        if (lo < hi) {
          submit_snapshot_scratch_.emplace_back(lo, hi - lo);
        }
      }
      upload_ranges_.swap(submit_snapshot_scratch_);
    }
  }
  // Only pages still committed: one drawn from a frame or two ago may have
  // been freed since (loading), and reading it faults. Checked here, not under
  // the heap lock through the copy. The on-demand path (no check; it copies
  // the pages a draw asks for) is exposed to a free before its copy too, but
  // this set (pages read two swaps ago, which a level load can free) makes a
  // free between this check and the copy likelier here.
  // Such a free can fault the copy only with protect_on_release on (default
  // off, src/system/xmemory.cpp), which makes a released page host NoAccess.
  memory::VirtualHeap* heap = memory().GetPhysicalHeap();
  auto readable = [&](uint32_t first, uint32_t count) {
    return heap->QueryRangeAccess(first << page_size_log2_,
                                  ((first + count) << page_size_log2_) - 1) !=
           rex::memory::PageAccess::kNoAccess;
  };
  prefetch_ranges_.clear();
  for (const auto& [first, count] : upload_ranges_) {
    if (readable(first, count)) {
      prefetch_ranges_.emplace_back(first, count);
      continue;
    }
    for (uint32_t p = first; p < first + count; ++p) {
      if (!readable(p, 1)) {
        continue;
      }
      if (!prefetch_ranges_.empty() &&
          prefetch_ranges_.back().first + prefetch_ranges_.back().second == p) {
        ++prefetch_ranges_.back().second;
      } else {
        prefetch_ranges_.emplace_back(p, 1);
      }
    }
  }
  uint32_t pages = 0;
  for (const auto& range : prefetch_ranges_) {
    pages += range.second;
  }
  if (!prefetch_ranges_.empty()) {
    UploadRanges(prefetch_ranges_);
  }
  return pages;
}

bool SharedMemory::AnyPageGpuWritten(uint32_t start, uint32_t length) const {
  if (!length || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + (length - 1)) >> page_size_log2_;
  const size_t words = system_page_flags_valid_and_gpu_written_.size();
  for (uint32_t w = page_first >> 6; w <= page_last >> 6 && w < words; ++w) {
    const uint32_t lo = w == page_first >> 6 ? page_first & 63 : 0;
    const uint32_t hi = w == page_last >> 6 ? page_last & 63 : 63;
    const uint64_t mask = (~uint64_t(0) >> (63 - (hi - lo))) << lo;
    if (system_page_flags_valid_and_gpu_written_[w] & mask) {
      return true;
    }
  }
  return false;
}

void SharedMemory::SubmitSnapshotInvalidateThunk(void* context, uint32_t start_bytes,
                                                 uint32_t length_bytes) {
  // D3: the path a CPU write takes (clears the valid and GPU-written bits, marks
  // the blocks dirty, fires the watches). Takes the global lock, which the
  // caller already holds (recursive).
  static_cast<SharedMemory*>(context)->MemoryInvalidationCallback(start_bytes, length_bytes, true);
}

bool SharedMemory::SubmitSnapshotGpuWrittenThunk(void* context, uint32_t address, uint32_t size) {
  // Called inside GpuFrameBoundary, under the global lock its caller holds.
  return static_cast<const SharedMemory*>(context)->AnyPageGpuWritten(address, size);
}

void SharedMemory::SubmitSnapshotFrameBoundary(uint32_t swap_ordinal, bool on) {
  // D6/W2: `on` is the cvar read live here, not the value VdSwap saw. A toggle between a VdSwap and its packet
  // gives one frame of live uploads (off at VdSwap, on here: no copy for that ordinal, counted missing_not_yet when
  // the lag marks gave the packet an ordinal; on at VdSwap, off here: latched off) and never a mixed frame;
  // accepted, only the F4 settings overlay toggles it.
  submit_snapshot::State& snapshot = submit_snapshot::Global();
  {
    // B2: one hold of the global lock for the whole boundary. Lock order: the
    // global lock, then the helper's publish mutex (inside); the guest side
    // (VdSwap) never takes the global lock while it holds swap_mutex().
    auto global_lock = global_critical_region_.Acquire();
    snapshot.GpuFrameBoundary(swap_ordinal, on, SubmitSnapshotInvalidateThunk, this,
                              SubmitSnapshotGpuWrittenThunk, this, page_size_log2_);
  }
  if (snapshot.gpu_on() && ++submit_snapshot_frames_on_ % 3600 == 0) {
    REXGPU_INFO("{}", snapshot.StatsLine());
  }
}

void SharedMemory::ResetSubmitSnapshot() {
  submit_snapshot::State& snapshot = submit_snapshot::Global();
  {
    // State::Reset drops the pages kept this frame without invalidating them, and
    // ClearCache leaves CPU pages valid when its refresh finds nothing dirty: so
    // every page of a learned range (within a frame the learned pages only grow,
    // so they cover every kept page) goes the way of a CPU write first.
    auto global_lock = global_critical_region_.Acquire();
    for (const auto& [first, count] : snapshot.LearnedPageSpans(page_size_log2_)) {
      MemoryInvalidationCallback(first << page_size_log2_, count << page_size_log2_, true);
    }
  }
  snapshot.Reset();
}

void SharedMemory::SpliceSubmitSnapshot(uint32_t start_bytes, uint32_t size_bytes, uint8_t* dest) {
  submit_snapshot::State& snapshot = submit_snapshot::Global();
  if (snapshot.gpu_on()) {
    snapshot.GpuSplice(start_bytes, size_bytes, dest, page_size_log2_);
  }
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  // TEMP DIAGNOSTIC (N2 probe): the refresh window before this callback loads
  // the active buffer, and the page the guest wrote (before the widening below,
  // which only adds pages whose contents did not change).
  const uint32_t refresh_window_before = refresh_in_window_.load(std::memory_order_seq_cst);
  const uint32_t probe_page = page_first;

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  uint32_t dirty_blocks_mask = 0;
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (valid_flags) {
      valid_flags[i] &= ~invalidate_bits;
    }
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
    dirty_blocks_mask |= uint32_t(1) << (i >> 6);
  }
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
  dirty_blocks_.fetch_or(dirty_blocks_mask, std::memory_order_relaxed);

  // TEMP DIAGNOSTIC (N2 probe): a hit if the window was open before or after
  // the clears above, or one opened and closed while they ran.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint32_t refresh_window_after = refresh_in_window_.load(std::memory_order_seq_cst);
  if ((refresh_window_before & 1) || (refresh_window_after & 1) ||
      refresh_window_before != refresh_window_after) {
    refresh_window_hits_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t probe_index = refresh_page_count_.fetch_add(1, std::memory_order_relaxed);
    if (probe_index < kRefreshProbePages) {
      refresh_pages_[probe_index].store(probe_page, std::memory_order_release);
    }
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();
  assert_true(trace_download_ranges_.empty());
  assert_zero(trace_download_page_count_);

  // Invalidate the entire memory CPU->GPU memory copy so all the history
  // doesn't have to be written into every frame trace, and collect the list of
  // ranges with data modified on the GPU.

  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t previously_valid_block = valid_flags ? valid_flags[i] : 0;
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    if (valid_flags) {
      valid_flags[i] = gpu_written_block;
    }

    // Fire watches on the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t fire_watches_block_page;
      if (!rex::bit_scan_forward(fire_watches_range_start == UINT32_MAX ? fire_watches_block
                                                                        : fire_watches_break_block,
                                 &fire_watches_block_page)) {
        break;
      }
      uint32_t fire_watches_page = (i << 6) + fire_watches_block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = fire_watches_page;
      } else {
        FireWatches(fire_watches_range_start, fire_watches_page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t fire_watches_block_mask = ~((uint64_t(1) << fire_watches_block_page) - 1);
      fire_watches_block &= fire_watches_block_mask;
      fire_watches_break_block &= fire_watches_block_mask;
    }

    // Add to the GPU-written ranges.
    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t gpu_written_block_page;
      if (!rex::bit_scan_forward(
              gpu_written_range_start == UINT32_MAX ? gpu_written_block : gpu_written_break_block,
              &gpu_written_block_page)) {
        break;
      }
      uint32_t gpu_written_page = (i << 6) + gpu_written_block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = gpu_written_page;
      } else {
        uint32_t gpu_written_range_length = gpu_written_page - gpu_written_range_start;
        // Call EnsureHostGpuMemoryAllocated in case the page was marked as
        // GPU-written not as a result to an actual write to the shared memory
        // buffer, but, for instance, by resolving with resolution scaling (to a
        // separate buffer).
        if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                         gpu_written_range_length << page_size_log2_)) {
          trace_download_ranges_.push_back(
              std::make_pair(gpu_written_range_start << page_size_log2_,
                             gpu_written_range_length << page_size_log2_));
          trace_download_page_count_ += gpu_written_range_length;
        }
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t gpu_written_block_mask = ~((uint64_t(1) << gpu_written_block_page) - 1);
      gpu_written_block &= gpu_written_block_mask;
      gpu_written_break_block &= gpu_written_block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    uint32_t gpu_written_range_length = page_count - gpu_written_range_start;
    if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                     gpu_written_range_length << page_size_log2_)) {
      trace_download_ranges_.push_back(std::make_pair(gpu_written_range_start << page_size_log2_,
                                                      gpu_written_range_length << page_size_log2_));
      trace_download_page_count_ += gpu_written_range_length;
    }
  }
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
