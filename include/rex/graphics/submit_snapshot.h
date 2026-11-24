/**
 * Submit snapshot (raptor centre seam fix): the GPU thread uploads learned vertex ranges from a copy taken at the
 * guest's VdSwap instead of from live guest memory, which the job workers may already be rewriting for the next
 * frame. Pure: no logging, cvar, memory or graphics headers, so tests/host/submit_snapshot_host_test.cpp builds it
 * alone. Design: notes/specs/2026-11-22-doa4-raptor-submit-snapshot-design.md (sections 1-4 and 6).
 *
 * Threads and locks (one sentence per shared object):
 * - Ring slots: the guest writes a slot and the GPU thread splices from it, each under that slot's own mutex.
 * - The published range list and its generation: written by the GPU thread whenever the learner's list changes,
 *   read by the guest in GuestSwap, under publish_mutex_ held only to copy the small vector (never during a byte
 *   copy); the generation is also an atomic so GuestSwap can skip the lock when nothing moved.
 * - Guest-side counters (NoteSwapThread, NoteCopyMicros, check sums): written by the guest (under swap_mutex() in
 *   the SDK), read by StatsLine on the GPU thread without any lock, so they are relaxed atomics; the two
 *   swap_threads ids are claimed by compare-exchange.
 * - Everything else (the learner, kept spans, GPU-side counters, the latch, the current frame) is GPU-thread only.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics::submit_snapshot {

inline constexpr uint32_t kRingDepth = 4, kMaxRanges = 512, kMaxTotalBytes = 512 * 1024,
                          kMaxRangeBytes = 64 * 1024, kAgeFrames = 120;
// Cap eviction (spec 2026-11-23-doa4-raptor-cap-evict-design.md G1, G2): a candidate the caps refused waits for the
// next BeginFrame (at most kMaxPending of them per boundary), which evicts listed ranges idle >= kEvictIdleFrames.
inline constexpr uint32_t kEvictIdleFrames = 8, kMaxPending = 16;

struct Range {
  uint32_t address;  // guest physical, bytes
  uint32_t size;
};

using ReadFn = bool (*)(void* ctx, uint32_t address, uint32_t size, uint8_t* dest);  // false: not readable
using InvalidateFn = void (*)(void* ctx, uint32_t start_bytes, uint32_t length_bytes);
using GpuWrittenFn = bool (*)(void* ctx, uint32_t address, uint32_t size);  // any page GPU-written

enum class Missing : uint8_t { kNone, kReused, kNotYet };

struct CopyResult {
  uint32_t copied, skipped_unreadable, bytes, check_moved;
};

struct SpliceResult {
  Missing missing;
  uint32_t spliced, differing, not_in_copy;
};

class State;

// Guest thread writes slots, GPU thread reads them; one std::mutex per slot (D4).
class Ring {
 public:
  Ring();  // reserves every slot: kMaxRanges entries, kMaxTotalBytes bytes
  // Copies every range of `list` into slot ordinal % kRingDepth and tags the slot with `ordinal` last. A range
  // `read` refuses is skipped (not held by the copy). `check`: re-reads each copied range right after the whole
  // copy and counts the ranges whose bytes moved (or could not be re-read).
  CopyResult Copy(uint32_t ordinal, std::span<const Range> list, ReadFn read, void* ctx, bool check);
  // dest holds guest bytes [start, start + size) copied from `live` (the same window of guest memory, the source of
  // the upload's memcpy); overwrites, inside dest, the bytes of copy `frame`'s ranges that also lie in a `learned`
  // range (a range the copy holds but the learner dropped stays live); appends each spliced piece (held and learned
  // and inside the window) whose copy bytes differ from live's bytes to *differing (cleared first). dest is only
  // written, never read (an upload mapping is write-combined or uncached memory). spliced / differing count those
  // pieces. kNone only when the slot's ordinal equals `frame`; greater -> kReused; smaller or never written ->
  // kNotYet; on a miss dest is untouched. not_in_copy (kNone only): learned ranges overlapping the window that no
  // single range of the copy contains.
  SpliceResult Splice(uint32_t frame, uint32_t start, uint32_t size, uint8_t* dest, const uint8_t* live,
                      std::span<const Range> learned, std::vector<Range>* differing);

 private:
  friend class State;
  struct Slot {
    std::mutex mutex;
    uint32_t ordinal = 0;  // 0: never written (ordinal 0 is never minted)
    std::vector<Range> ranges;
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> check_scratch;
  };
  void ResetOrdinals();
  std::array<Slot, kRingDepth> slots_;
};

// GPU thread only.
class Learner {
 public:
  Learner();
  // Ages (D11), then admits the candidates the caps refused since the last BeginFrame (pending; cap eviction), then
  // clears the frame's notes and pass table.
  void BeginFrame(uint32_t frame);
  void NoteRequest(uint32_t address, uint32_t size, uint64_t bin_select, uint32_t uploaded_pages);
  void NoteChanged(const Range& r);  // a splice saw copy != live inside r: last_active = frame
  // Drops every listed range with a GPU-written page and bans the whole pages it covers (page size
  // 1 << page_size_log2); a candidate overlapping a banned page is refused. A17: a ban leaves after kAgeFrames
  // frames without its pages being GPU-written; at most kMaxRanges bans. Also re-tests and ages existing bans.
  void BanOverlapping(GpuWrittenFn written, void* ctx, uint32_t page_size_log2);
  std::span<const Range> ranges() const;  // sorted by address, merged
  uint64_t generation() const;            // +1 on every change of ranges()
  const std::vector<std::pair<uint32_t, uint32_t>>& LearnedPageSpans(uint32_t page_size_log2);  // (first, count)
  uint64_t evicted() const;  // ranges removed to admit a pending candidate (the stats line's evicted=)

 private:
  friend class State;
  struct Note {
    uint32_t address;
    uint32_t epoch;  // the slot is empty unless it equals epoch_
    uint32_t first_pass;
  };
  struct Ban {
    Range range;
    uint32_t last_written;
  };
  static constexpr uint32_t kNoteTableLog2 = 14;
  static constexpr uint32_t kMaxNotes = (1u << kNoteTableLog2) / 4 * 3;
  static constexpr uint32_t kMaxPasses = 64;
  uint32_t PassIndex(uint64_t bin_select);
  void TryLearn(Range c);
  // BeginFrame, after aging: admits each pending candidate in order and empties pending_. A candidate that does not
  // fit is dropped with no eviction when it would not fit even with every eligible range gone (R3-pre: ranges outside
  // its overlap, idle >= kEvictIdleFrames); otherwise the longest-idle eligible ranges are evicted until it fits.
  // True when the list changed (an eviction or an insert).
  bool AdmitPending();
  void RemoveAt(size_t i);
  bool Banned(const Range& c) const;
  void AddBan(const Range& pages);

  uint32_t frame_ = 0;
  uint64_t generation_ = 0;
  std::vector<Range> list_;
  std::vector<uint32_t> last_active_;
  uint64_t total_bytes_ = 0;
  std::vector<Note> notes_;
  uint32_t epoch_ = 1;
  uint32_t note_count_ = 0;
  std::array<uint64_t, kMaxPasses> passes_{};
  uint32_t pass_count_ = 0;
  std::vector<Ban> bans_;
  std::vector<std::pair<uint32_t, uint32_t>> spans_;
  // Refusal events (A16), read by State::StatsLine.
  uint64_t rejected_cap_ = 0, rejected_size_ = 0, banned_ = 0;
  // Cap eviction: the candidates the caps refused since the last BeginFrame (deduplicated, at most kMaxPending),
  // admitted or dropped at the next BeginFrame; evicted_ and the window's first refusal (State::StatsLine, survive
  // Reset).
  std::array<Range, kMaxPending> pending_{};
  uint32_t pending_count_ = 0;
  uint64_t evicted_ = 0;
  Range refused_first_{};
  uint8_t refused_kind_ = 0;  // 0 none in this window, 1 count, 2 bytes
};

// Process-wide via Global(); instantiable for tests.
class State {
 public:
  State();
  // guest (VdSwap): under swap_mutex() when on; refreshes its list copy if the learner's published generation moved
  CopyResult GuestSwap(uint32_t ordinal, ReadFn read, void* ctx, bool check);
  std::mutex& swap_mutex();
  void NoteSwapThread(uint64_t thread_id);  // swap_threads= counts (D8)
  void NoteCopyMicros(uint32_t us);         // histogram, 1 us buckets to 2000, then one overflow bucket
  // GPU thread
  void GpuFrameBoundary(uint32_t swap_ordinal, bool on, InvalidateFn inv, void* inv_ctx, GpuWrittenFn written,
                        void* written_ctx, uint32_t page_size_log2);  // kept spans invalidated FIRST, whatever `on`
  bool gpu_on() const;                       // the latch (D6)
  void GpuNoteRequest(uint32_t address, uint32_t size, uint64_t bin_select, uint32_t uploaded_pages);
  // dest and live as Ring::Splice. No-op unless latched on and a current frame exists; differing -> kept spans +
  // NoteChanged.
  void GpuSplice(uint32_t start, uint32_t size, uint8_t* dest, const uint8_t* live, uint32_t page_size_log2);
  const std::vector<std::pair<uint32_t, uint32_t>>& LearnedPageSpans(uint32_t page_size_log2);
  // GPU thread (review N1): kept or learned pages that a kept-span or reset invalidation skipped because they were
  // GPU-written (ForEachClearPageRun's return); the stats line's kept_gpu_written. Survives Reset.
  void NoteKeptGpuWritten(uint32_t pages);
  // The "submit snapshot stats: " line, fields as Task 2 Step 6 (+ A16's three), ending kept_gpu_written, evicted,
  // ranges_bytes, refused_first.
  std::string StatsLine() const;
  // GPU thread: starts a new refusal window (refused_first= reads "-" until the next cap refusal); the periodic
  // stats print calls it right after printing (the teardown print does not). Reset keeps the window's refusal.
  void ClearWindowRefusal();
  // GPU thread (cache clear): clears the learner, the bans, the published list (generation moves), the ring slots'
  // ordinals (to "never written") and the kept spans; the caller invalidates all pages itself.
  void Reset();

 private:
  static constexpr uint32_t kHistBuckets = 2002;  // 0..2000 us, then the overflow bucket (reads as 2001)
  static constexpr uint64_t kNoThread = ~uint64_t(0);
  void PublishIfChanged();
  void Publish();
  void AddKeptSpan(uint64_t start, uint64_t end);

  // GPU thread.
  Learner learner_;
  uint64_t learner_gen_seen_ = 0;
  bool on_ = false;
  uint32_t frame_ = 0;  // 0: no current frame
  std::vector<std::pair<uint64_t, uint64_t>> kept_;  // [start, end) bytes, page-aligned, sorted, merged
  std::vector<Range> differing_;
  uint64_t frames_ = 0, ranges_max_ = 0, spliced_ = 0, differing_calls_ = 0, missing_reused_ = 0,
           missing_not_yet_ = 0, not_in_copy_ = 0;
  uint64_t kept_gpu_written_ = 0;  // NoteKeptGpuWritten (SDK: called under the global lock as well)

  // Published list (GPU writes, guest reads).
  std::mutex publish_mutex_;
  std::vector<Range> published_;
  std::atomic<uint64_t> publish_gen_{0};

  // Guest.
  std::mutex swap_mutex_;
  std::vector<Range> guest_list_;
  uint64_t guest_gen_ = 0;
  Ring ring_;
  std::array<std::atomic<uint32_t>, kHistBuckets> hist_{};
  std::atomic<uint64_t> copy_n_{0};
  std::atomic<uint32_t> copy_max_{0};
  std::atomic<uint64_t> check_ranges_{0}, check_moved_{0};
  std::array<std::atomic<uint64_t>, 2> thread_ids_{};
  std::array<std::atomic<uint64_t>, 2> thread_counts_{};
  std::atomic<uint64_t> thread_other_{0};
};

State& Global();

// Calls run(ctx, first, count) for each maximal run of pages in [page_first, page_first + page_count) whose bit in
// `bits` is clear (page p is bit p & 63 of word p >> 6: SharedMemory's page flag blocks of 64 pages), in page order.
// Pages past the last word are not visited. Returns the number of pages of the visited (clamped) span whose bit is
// set, i.e. the pages it skipped. For the kept-span and reset invalidations (review M2): a page whose GPU-written bit
// is set keeps its data, the rest go the way of a CPU write.
using PageRunFn = void (*)(void* ctx, uint32_t page_first, uint32_t page_count);
uint32_t ForEachClearPageRun(std::span<const uint64_t> bits, uint32_t page_first, uint32_t page_count, PageRunFn run,
                             void* ctx);

}  // namespace rex::graphics::submit_snapshot
