#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/thread/mutex.h>

namespace rex::graphics {

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  void InvalidateAllPages();

  // TEMP DIAGNOSTIC (N2 probe): CPU writes that land inside the frame-end
  // valid-flag refresh (which now holds the global lock, so both counters
  // should stay 0). Counted since the last TakeRefreshProbe: window_hits =
  // invalidation callbacks that saw the refresh window open, lost = stored
  // pages left valid in the new active buffer although cleared in the
  // GPU-written flags, stretches = refreshes that waited
  // gpu_refresh_stretch_ms between their copy and the exchange, lock_us_max =
  // the longest time one refresh held the global lock (from right after its
  // Acquire to its end, the stretch's measured duration subtracted; the
  // Acquire's own wait is not in it), in microseconds.
  struct RefreshProbe {
    uint64_t window_hits;
    uint64_t lost;
    uint64_t stretches;
    double lock_us_max;
  };
  // Returns the counters and zeroes them.
  RefreshProbe TakeRefreshProbe();

  typedef void (*GlobalWatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                      void* context, uint32_t address_first, uint32_t address_last,
                                      bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable. uploaded_pages (optional):
  // the number of pages handed to UploadRanges by this call (0 when every page
  // was valid already, and on every early return).
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                     uint32_t* uploaded_pages = nullptr);
  bool RequestRange(uint32_t start, uint32_t length, uint32_t* uploaded_pages = nullptr);

  // Submit snapshot (submit_snapshot.h, gpu_submit_snapshot): the GPU thread's
  // frame boundary, at each swap packet (CommandProcessor::
  // OnSubmitSnapshotFrameBoundary). swap_ordinal = the packet's VdSwap ordinal
  // (0: none); on = gpu_submit_snapshot read now (the latch for the frame).
  // Holds the global lock through the whole call (the pages kept from the last
  // frame are invalidated through MemoryInvalidationCallback, the GPU-written
  // bans read system_page_flags_valid_and_gpu_written_). GPU thread only.
  void SubmitSnapshotFrameBoundary(uint32_t swap_ordinal, bool on);
  // Submit snapshot at a cache clear (after ClearCache, GPU thread):
  // invalidates every page of a learned range (a superset of the pages kept
  // this frame), then State::Reset.
  void ResetSubmitSnapshot();

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  void RangeWrittenByGpu(uint32_t start, uint32_t length);

  // Uploads in one batch, and marks valid, the pages draws read two frames
  // (swaps) ago that are invalid at this point, rather than draw by draw: the
  // game's per-frame data (dynamic meshes, double-buffered) is written ahead
  // of the frame drawing it, and fetched draw by draw it cost a copy and a
  // barrier pair each. Two frames ago, not the last one: a double buffer's
  // half the last frame read is already being rewritten for the frame after
  // next. Pages holding GPU-written data are valid, so never uploaded. Which
  // pages are invalid depends on clear_memory_page_state:
  // - On (the default): the frame-end refresh that runs right before this
  //   (SetSystemPageBlocksValidWithGpuDataWritten) resets the pages the GPU
  //   did not write to invalid, in the blocks it copies. So this re-uploads
  //   every page of the set that the refresh reset, whether the CPU wrote it
  //   again or not, and marks it valid again. From this upload until the draw
  //   that reads the page, a CPU write to it is caught only by the write
  //   watch; without the prefetch, the draw would re-read the page from CPU
  //   memory.
  // - Off: only the pages the write watch invalidated, the ones the CPU has
  //   written again since; a write after this upload is caught by the watch,
  //   as on demand.
  // Call at the end of a frame, in an open submission, before the next
  // frame's first command. Returns the number of pages uploaded. Ported from
  // lmaogoodcodenotreally/rexglue-sdk@59e13bba (gpu_upload_prefetch, off by
  // default here).
  bool UploadPrefetchEnabled() const;
  uint32_t PrefetchFrameUploads();

 protected:
  SharedMemory(memory::Memory& memory);
  // Call in implementation-specific initialization.
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  memory::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;

  // Submit snapshot: dest holds guest bytes [start_bytes, start_bytes +
  // size_bytes) just copied from guest memory into an upload buffer; overwrites
  // the bytes of learned ranges with the current frame's VdSwap copy (a no-op
  // unless the frame is latched on and has a copy). Call in UploadRanges right
  // after each memcpy, without the global lock held. GPU thread only.
  void SpliceSubmitSnapshot(uint32_t start_bytes, uint32_t size_bytes, uint8_t* dest);

  const std::vector<std::pair<uint32_t, uint32_t>>& trace_download_ranges() {
    return trace_download_ranges_;
  }
  uint32_t trace_download_page_count() const { return trace_download_page_count_; }
  // Fills trace_download_ranges() and trace_download_page_count() with
  // GPU-written ranges that need to be downloaded, and also invalidates
  // non-GPU-written ranges so only the needed data - not the all the collected
  // data - will be written in the trace. trace_download_page_count() will be 0
  // if nothing to download.
  void PrepareForTraceDownload();
  // Release memory used for trace download ranges, to be called after
  // downloading or in cases when download is dropped.
  void ReleaseTraceDownloadRanges();

 private:
  memory::Memory& memory_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;

  // Ranges that need to be uploaded, generated by GetRangesToUpload (a
  // persistently allocated vector).
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // GPU-written memory downloading for traces. <Start address, length>.
  std::vector<std::pair<uint32_t, uint32_t>> trace_download_ranges_;
  uint32_t trace_download_page_count_ = 0;

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  rex::thread::global_critical_region global_critical_region_;

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  // Double-buffered valid-page flags for lockless checks in RequestRanges.
  std::vector<uint64_t> valid_buffer_a_;
  std::vector<uint64_t> valid_buffer_b_;
  std::atomic<uint64_t*> active_valid_flags_{nullptr};
  std::atomic<uint64_t*> staging_valid_flags_{nullptr};
  // Pages requested (read by the GPU) this frame, the frame before, and the
  // one before that. Requests, not uploads: pages the prefetch uploaded but
  // nothing read must drop out. Used by the GPU command thread only.
  std::vector<uint64_t> upload_history_[3];
  std::vector<std::pair<uint32_t, uint32_t>> prefetch_ranges_;  // pages
  void NoteRequested(uint32_t start, uint32_t length);          // bytes
  // Submit snapshot (GPU thread): the frames latched on since start (the stats
  // line every 3600 of them), and the prefetch's ranges minus learned pages.
  uint64_t submit_snapshot_frames_on_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> submit_snapshot_scratch_;  // pages
  // Call with the global lock held: any page of [start, start + length) holds
  // GPU-written data (system_page_flags_valid_and_gpu_written_).
  bool AnyPageGpuWritten(uint32_t start, uint32_t length) const;
  static void SubmitSnapshotInvalidateThunk(void* context, uint32_t start_bytes,
                                            uint32_t length_bytes);
  static bool SubmitSnapshotGpuWrittenThunk(void* context, uint32_t address, uint32_t size);
  // Subset of valid pages containing data written by the GPU.
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  // Dirty state tracking for frame-end page-state refresh.
  std::atomic<bool> gpu_written_data_dirty_{false};
  std::atomic<uint32_t> dirty_blocks_{0};
  uint32_t num_system_page_flags_ = 0;

  // TEMP DIAGNOSTIC (N2 probe). refresh_in_window_ is odd while the frame-end
  // refresh is between the start of its copy and its last flag store (the
  // refresh adds 1 when it opens and 1 when it closes), so an invalidation
  // callback that reads it before and after its clears also sees a window it
  // straddled. The callback (under the global lock) stores the first page it
  // was asked to invalidate into refresh_pages_ (kNoRefreshPage = not stored
  // yet); the refresh walks them after it closes the window, still under the
  // global lock it now holds for its whole body.
  static constexpr uint32_t kRefreshProbePages = 64;
  static constexpr uint32_t kNoRefreshPage = UINT32_MAX;
  std::atomic<uint32_t> refresh_in_window_{0};
  std::atomic<uint32_t> refresh_page_count_{0};
  std::atomic<uint32_t> refresh_pages_[kRefreshProbePages] = {};
  std::atomic<uint64_t> refresh_window_hits_{0};
  std::atomic<uint64_t> refresh_lost_{0};
  std::atomic<uint64_t> refresh_stretches_{0};
  // TEMP DIAGNOSTIC: the longest global-lock hold of one refresh since the last
  // TakeRefreshProbe, in Clock::QueryHostTickCount ticks (stretch excluded).
  std::atomic<uint64_t> refresh_lock_ticks_max_{0};
  void NoteRefreshLockHold(uint64_t ticks);

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::graphics
