/**
 * Submit snapshot helper (raptor centre seam fix). See include/rex/graphics/submit_snapshot.h for the threads and
 * locks; tests/host/submit_snapshot_host_test.cpp for the rules, one group each.
 *
 * No allocation after warm-up on the per-frame paths (GuestSwap, GpuSplice, GpuNoteRequest, GpuFrameBoundary):
 * every vector is reserved to its cap in a constructor and only cleared, assigned or inserted into within it.
 */
#include <rex/graphics/submit_snapshot.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rex::graphics::submit_snapshot {

namespace {

uint64_t End(const Range& r) { return uint64_t(r.address) + r.size; }

bool Overlaps(const Range& r, uint64_t start, uint64_t end) { return r.address < end && start < End(r); }

// Frames passed since `then` (signed: a frame before `then` is never "aged").
bool AgedOut(uint32_t now, uint32_t then) { return int32_t(now - then) > int32_t(kAgeFrames); }

// Index of the first range of a sorted, non-overlapping list that ends after `start`.
size_t FirstEndingAfter(std::span<const Range> list, uint64_t start) {
  auto it = std::partition_point(list.begin(), list.end(), [start](const Range& r) { return End(r) <= start; });
  return size_t(it - list.begin());
}

bool AnyOverlap(std::span<const Range> list, uint64_t start, uint64_t end) {
  size_t i = FirstEndingAfter(list, start);
  return i < list.size() && list[i].address < end;
}

}  // namespace

// ---- Ring ---------------------------------------------------------------------------------------------------------

Ring::Ring() {
  for (Slot& s : slots_) {
    s.ranges.reserve(kMaxRanges);
    s.offsets.reserve(kMaxRanges);
    s.bytes.resize(kMaxTotalBytes);
    s.check_scratch.resize(kMaxRangeBytes);
  }
}

CopyResult Ring::Copy(uint32_t ordinal, std::span<const Range> list, ReadFn read, void* ctx, bool check) {
  CopyResult res{0, 0, 0, 0};
  Slot& s = slots_[ordinal % kRingDepth];
  std::lock_guard<std::mutex> lock(s.mutex);
  s.ranges.clear();
  s.offsets.clear();
  uint32_t off = 0;
  for (const Range& r : list) {
    if (r.size == 0) continue;
    // The learner's caps keep a published list inside the reserve; a list past it is cut, never grown into.
    if (s.ranges.size() >= kMaxRanges || r.size > kMaxTotalBytes - off) break;
    if (!read(ctx, r.address, r.size, s.bytes.data() + off)) {
      ++res.skipped_unreadable;
      continue;
    }
    s.ranges.push_back(r);
    s.offsets.push_back(off);
    off += r.size;
    ++res.copied;
    res.bytes += r.size;
  }
  if (check) {
    for (size_t i = 0; i < s.ranges.size(); ++i) {
      const Range& r = s.ranges[i];
      bool moved = false;
      for (uint32_t pos = 0; pos < r.size && !moved; pos += kMaxRangeBytes) {
        uint32_t n = std::min(kMaxRangeBytes, r.size - pos);
        if (!read(ctx, r.address + pos, n, s.check_scratch.data()) ||
            std::memcmp(s.check_scratch.data(), s.bytes.data() + s.offsets[i] + pos, n) != 0) {
          moved = true;
        }
      }
      if (moved) ++res.check_moved;
    }
  }
  s.ordinal = ordinal;  // tagged last
  return res;
}

SpliceResult Ring::Splice(uint32_t frame, uint32_t start, uint32_t size, uint8_t* dest,
                          std::span<const Range> learned, std::vector<Range>* differing) {
  SpliceResult res{Missing::kNone, 0, 0, 0};
  if (differing) differing->clear();
  Slot& s = slots_[frame % kRingDepth];
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.ordinal != frame) {
    res.missing = s.ordinal > frame ? Missing::kReused : Missing::kNotYet;
    return res;
  }
  const uint64_t w0 = start, w1 = uint64_t(start) + size;
  std::span<const Range> held(s.ranges);
  for (size_t i = FirstEndingAfter(held, w0); i < held.size() && held[i].address < w1; ++i) {
    const Range& r = held[i];
    uint64_t lo = std::max<uint64_t>(r.address, w0), hi = std::min(End(r), w1);
    if (lo >= hi) continue;
    const uint8_t* src = s.bytes.data() + s.offsets[i] + (lo - r.address);
    uint8_t* dst = dest + (lo - w0);
    size_t n = size_t(hi - lo);
    if (std::memcmp(src, dst, n) != 0) {
      ++res.differing;
      if (differing) differing->push_back(r);
    }
    std::memcpy(dst, src, n);
    ++res.spliced;
  }
  for (size_t i = FirstEndingAfter(learned, w0); i < learned.size() && learned[i].address < w1; ++i) {
    const Range& l = learned[i];
    // Held when one range of the copy contains it (the copy's ranges are sorted and do not overlap).
    size_t j = FirstEndingAfter(held, l.address);
    bool in_copy = j < held.size() && held[j].address <= l.address && End(held[j]) >= End(l);
    if (!in_copy) ++res.not_in_copy;
  }
  return res;
}

void Ring::ResetOrdinals() {
  for (Slot& s : slots_) {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.ordinal = 0;
    s.ranges.clear();
    s.offsets.clear();
  }
}

// ---- Learner ------------------------------------------------------------------------------------------------------

Learner::Learner() {
  list_.reserve(kMaxRanges);
  last_active_.reserve(kMaxRanges);
  notes_.assign(size_t(1) << kNoteTableLog2, Note{0, 0, 0});
  bans_.reserve(kMaxRanges);
  spans_.reserve(kMaxRanges);
}

void Learner::BeginFrame(uint32_t frame) {
  frame_ = frame;
  bool removed = false;
  for (size_t i = list_.size(); i-- > 0;) {
    if (AgedOut(frame, last_active_[i])) {
      RemoveAt(i);
      removed = true;
    }
  }
  if (removed) ++generation_;
  // Clear the notes by moving the epoch; on its wrap, empty the table for real.
  if (++epoch_ == 0) {
    for (Note& n : notes_) n.epoch = 0;
    epoch_ = 1;
  }
  note_count_ = 0;
  pass_count_ = 0;
}

uint32_t Learner::PassIndex(uint64_t bin_select) {
  for (uint32_t i = 0; i < pass_count_; ++i) {
    if (passes_[i] == bin_select) return i;
  }
  if (pass_count_ < kMaxPasses) {
    passes_[pass_count_] = bin_select;
    return pass_count_++;
  }
  return kMaxPasses;  // every pass past the table shares the last index
}

void Learner::NoteRequest(uint32_t address, uint32_t size, uint64_t bin_select, uint32_t uploaded_pages) {
  if (size == 0) return;
  uint32_t pass = PassIndex(bin_select);
  const uint32_t mask = (1u << kNoteTableLog2) - 1;
  uint32_t idx = (address * 2654435761u) >> (32 - kNoteTableLog2);
  for (;;) {
    Note& n = notes_[idx];
    if (n.epoch != epoch_) {
      // First request of this address this frame: remember its pass (a full table notes nothing more).
      if (note_count_ < kMaxNotes) {
        n = Note{address, epoch_, pass};
        ++note_count_;
      }
      return;
    }
    if (n.address == address) {
      if (uploaded_pages != 0 && n.first_pass < pass) TryLearn(Range{address, size});
      return;
    }
    idx = (idx + 1) & mask;
  }
}

bool Learner::Banned(const Range& c) const {
  for (const Ban& b : bans_) {
    if (Overlaps(b.range, c.address, End(c))) return true;
  }
  return false;
}

void Learner::TryLearn(Range c) {
  if (c.size > kMaxRangeBytes) {
    ++rejected_size_;
    return;
  }
  if (Banned(c)) {
    ++banned_;
    return;
  }
  // The ranges c overlaps: [first, last).
  const uint64_t c_end = End(c);
  size_t first = FirstEndingAfter(list_, c.address);
  size_t last = first;
  uint64_t lo = c.address, hi = c_end, removed_bytes = 0;
  while (last < list_.size() && list_[last].address < c_end) {
    lo = std::min<uint64_t>(lo, list_[last].address);
    hi = std::max(hi, End(list_[last]));
    removed_bytes += list_[last].size;
    ++last;
  }
  if (last - first == 1 && list_[first].address == lo && End(list_[first]) == hi) return;  // already held
  const size_t count_after = list_.size() - (last - first) + 1;
  const uint64_t bytes_after = total_bytes_ - removed_bytes + (hi - lo);
  if (count_after > kMaxRanges || bytes_after > kMaxTotalBytes) {
    ++rejected_cap_;
    return;
  }
  list_.erase(list_.begin() + first, list_.begin() + last);
  last_active_.erase(last_active_.begin() + first, last_active_.begin() + last);
  list_.insert(list_.begin() + first, Range{uint32_t(lo), uint32_t(hi - lo)});
  last_active_.insert(last_active_.begin() + first, frame_);
  total_bytes_ = bytes_after;
  ++generation_;
}

void Learner::RemoveAt(size_t i) {
  total_bytes_ -= list_[i].size;
  list_.erase(list_.begin() + i);
  last_active_.erase(last_active_.begin() + i);
}

void Learner::NoteChanged(const Range& r) {
  for (size_t i = FirstEndingAfter(list_, r.address); i < list_.size() && list_[i].address < End(r); ++i) {
    last_active_[i] = frame_;
  }
}

void Learner::AddBan(const Range& r) {
  if (bans_.size() < kMaxRanges) {
    bans_.push_back(Ban{r, frame_});
    return;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < bans_.size(); ++i) {
    if (int32_t(bans_[i].last_written - bans_[oldest].last_written) < 0) oldest = i;
  }
  bans_[oldest] = Ban{r, frame_};
}

void Learner::BanOverlapping(GpuWrittenFn written, void* ctx) {
  if (!written) return;
  // Re-test the bans: still written -> seen now; not written for more than kAgeFrames frames -> leaves.
  for (size_t i = bans_.size(); i-- > 0;) {
    Ban& b = bans_[i];
    if (written(ctx, b.range.address, b.range.size)) {
      b.last_written = frame_;
    } else if (AgedOut(frame_, b.last_written)) {
      bans_.erase(bans_.begin() + i);
    }
  }
  bool removed = false;
  for (size_t i = list_.size(); i-- > 0;) {
    if (written(ctx, list_[i].address, list_[i].size)) {
      AddBan(list_[i]);
      RemoveAt(i);
      ++banned_;
      removed = true;
    }
  }
  if (removed) ++generation_;
}

std::span<const Range> Learner::ranges() const { return list_; }

uint64_t Learner::generation() const { return generation_; }

const std::vector<std::pair<uint32_t, uint32_t>>& Learner::LearnedPageSpans(uint32_t page_size_log2) {
  spans_.clear();
  for (const Range& r : list_) {
    uint32_t first = r.address >> page_size_log2;
    uint32_t last = uint32_t((End(r) - 1) >> page_size_log2);
    if (!spans_.empty() && spans_.back().first + spans_.back().second >= first) {
      uint32_t end = std::max(spans_.back().first + spans_.back().second, last + 1);
      spans_.back().second = end - spans_.back().first;
    } else {
      spans_.emplace_back(first, last - first + 1);
    }
  }
  return spans_;
}

// ---- State --------------------------------------------------------------------------------------------------------

State::State() {
  kept_.reserve(kMaxRanges * 4);
  differing_.reserve(kMaxRanges);
  published_.reserve(kMaxRanges);
  guest_list_.reserve(kMaxRanges);
  for (auto& id : thread_ids_) id.store(kNoThread, std::memory_order_relaxed);
}

CopyResult State::GuestSwap(uint32_t ordinal, ReadFn read, void* ctx, bool check) {
  if (publish_gen_.load(std::memory_order_acquire) != guest_gen_) {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    guest_list_.assign(published_.begin(), published_.end());
    guest_gen_ = publish_gen_.load(std::memory_order_relaxed);
  }
  CopyResult r = ring_.Copy(ordinal, guest_list_, read, ctx, check);
  if (check) {
    check_ranges_.fetch_add(r.copied, std::memory_order_relaxed);
    check_moved_.fetch_add(r.check_moved, std::memory_order_relaxed);
  }
  return r;
}

std::mutex& State::swap_mutex() { return swap_mutex_; }

void State::NoteSwapThread(uint64_t thread_id) {
  for (size_t i = 0; i < thread_ids_.size(); ++i) {
    uint64_t seen = thread_ids_[i].load(std::memory_order_relaxed);
    if (seen == kNoThread && thread_id != kNoThread &&
        thread_ids_[i].compare_exchange_strong(seen, thread_id, std::memory_order_relaxed)) {
      seen = thread_id;
    }
    if (seen == thread_id) {
      thread_counts_[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  thread_other_.fetch_add(1, std::memory_order_relaxed);
}

void State::NoteCopyMicros(uint32_t us) {
  hist_[std::min<uint32_t>(us, kHistBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
  copy_n_.fetch_add(1, std::memory_order_relaxed);
  uint32_t m = copy_max_.load(std::memory_order_relaxed);
  while (us > m && !copy_max_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
  }
}

void State::Publish() {
  {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    std::span<const Range> list = learner_.ranges();
    published_.assign(list.begin(), list.end());
    publish_gen_.store(publish_gen_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  learner_gen_seen_ = learner_.generation();
  ranges_max_ = std::max<uint64_t>(ranges_max_, learner_.ranges().size());
}

void State::PublishIfChanged() {
  if (learner_.generation() != learner_gen_seen_) Publish();
}

void State::GpuFrameBoundary(uint32_t swap_ordinal, bool on, InvalidateFn inv, void* inv_ctx, GpuWrittenFn written,
                             void* written_ctx) {
  if (inv) {
    for (const auto& k : kept_) inv(inv_ctx, uint32_t(k.first), uint32_t(k.second - k.first));
  }
  kept_.clear();
  on_ = on;
  frame_ = (on && swap_ordinal != 0) ? swap_ordinal + 1 : 0;
  if (frame_ == 0) return;
  ++frames_;
  learner_.BeginFrame(frame_);
  learner_.BanOverlapping(written, written_ctx);
  PublishIfChanged();
}

bool State::gpu_on() const { return on_; }

void State::GpuNoteRequest(uint32_t address, uint32_t size, uint64_t bin_select, uint32_t uploaded_pages) {
  if (!on_ || frame_ == 0) return;
  learner_.NoteRequest(address, size, bin_select, uploaded_pages);
  PublishIfChanged();
}

void State::AddKeptSpan(uint64_t start, uint64_t end) {
  // Sorted, merged when overlapping or touching. At the reserve's end the nearest span widens instead (more pages
  // invalidated than needed, never fewer).
  auto it = std::partition_point(kept_.begin(), kept_.end(), [start](const auto& k) { return k.second < start; });
  if (it != kept_.end() && it->first <= end) {
    it->first = std::min(it->first, start);
    it->second = std::max(it->second, end);
    auto next = it + 1;
    while (next != kept_.end() && next->first <= it->second) {
      it->second = std::max(it->second, next->second);
      next = kept_.erase(next);
    }
    return;
  }
  if (kept_.size() == kept_.capacity()) {
    auto& k = it != kept_.end() ? *it : kept_.back();
    k.first = std::min(k.first, start);
    k.second = std::max(k.second, end);
    return;
  }
  kept_.insert(it, {start, end});
}

void State::GpuSplice(uint32_t start, uint32_t size, uint8_t* dest, uint32_t page_size_log2) {
  if (!on_ || frame_ == 0 || size == 0) return;
  std::span<const Range> learned = learner_.ranges();
  const uint64_t w0 = start, w1 = uint64_t(start) + size;
  if (!AnyOverlap(learned, w0, w1)) return;
  SpliceResult r = ring_.Splice(frame_, start, size, dest, learned, &differing_);
  if (r.missing == Missing::kReused) {
    ++missing_reused_;
    return;
  }
  if (r.missing == Missing::kNotYet) {
    ++missing_not_yet_;
    return;
  }
  not_in_copy_ += r.not_in_copy;
  if (r.spliced == 0) return;
  ++spliced_;
  if (r.differing == 0) return;
  ++differing_calls_;
  const uint64_t page = uint64_t(1) << page_size_log2;
  for (const Range& d : differing_) {
    learner_.NoteChanged(d);
    uint64_t lo = std::max<uint64_t>(d.address, w0), hi = std::min(End(d), w1);
    AddKeptSpan(lo & ~(page - 1), (hi + page - 1) & ~(page - 1));
  }
}

const std::vector<std::pair<uint32_t, uint32_t>>& State::LearnedPageSpans(uint32_t page_size_log2) {
  return learner_.LearnedPageSpans(page_size_log2);
}

std::string State::StatsLine() const {
  uint64_t n = copy_n_.load(std::memory_order_relaxed);
  uint64_t median = 0;
  if (n != 0) {
    uint64_t k = (n + 1) / 2, cum = 0;  // the ceil(n/2)-th smallest sample (lower median)
    for (uint32_t b = 0; b < kHistBuckets; ++b) {
      cum += hist_[b].load(std::memory_order_relaxed);
      if (cum >= k) {
        median = b;  // the overflow bucket is index 2001 and reads as 2001
        break;
      }
    }
  }
  std::string threads;
  char buf[96];
  for (size_t i = 0; i < thread_ids_.size(); ++i) {
    uint64_t id = thread_ids_[i].load(std::memory_order_relaxed);
    if (id == kNoThread) continue;
    std::snprintf(buf, sizeof(buf), "%llu:%llu,", static_cast<unsigned long long>(id),
                  static_cast<unsigned long long>(thread_counts_[i].load(std::memory_order_relaxed)));
    threads += buf;
  }
  std::snprintf(buf, sizeof(buf), "other:%llu",
                static_cast<unsigned long long>(thread_other_.load(std::memory_order_relaxed)));
  threads += buf;
  auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
  char line[640];
  std::snprintf(line, sizeof(line),
                "submit snapshot stats: frames=%llu ranges_max=%llu ranges_now=%llu spliced=%llu differing=%llu "
                "missing_reused=%llu missing_not_yet=%llu not_in_copy=%llu copy_us_median=%llu copy_us_max=%llu "
                "check_ranges=%llu check_moved=%llu swap_threads=%s rejected_cap=%llu rejected_size=%llu banned=%llu",
                u(frames_), u(ranges_max_), u(learner_.ranges().size()), u(spliced_), u(differing_calls_),
                u(missing_reused_), u(missing_not_yet_), u(not_in_copy_), u(median),
                u(copy_max_.load(std::memory_order_relaxed)), u(check_ranges_.load(std::memory_order_relaxed)),
                u(check_moved_.load(std::memory_order_relaxed)), threads.c_str(), u(learner_.rejected_cap_),
                u(learner_.rejected_size_), u(learner_.banned_));
  return line;
}

void State::Reset() {
  kept_.clear();
  // The refusal counters are statistics of the run, not learned state: they survive the reset.
  uint64_t cap = learner_.rejected_cap_, size = learner_.rejected_size_, banned = learner_.banned_;
  learner_ = Learner();
  learner_.rejected_cap_ = cap;
  learner_.rejected_size_ = size;
  learner_.banned_ = banned;
  Publish();  // the empty list, generation moved
  ring_.ResetOrdinals();
}

State& Global() {
  static State s;
  return s;
}

}  // namespace rex::graphics::submit_snapshot
