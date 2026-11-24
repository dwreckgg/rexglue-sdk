// Host test for the submit snapshot helper (include/rex/graphics/submit_snapshot.h,
// src/graphics/submit_snapshot.cpp): the ring of VdSwap copies, the range learner and the GPU-side frame state that
// Task 2 wires into the command processors (the raptor centre seam fix). Built and run by
// run_submit_snapshot_test.bat; no CMake, no Catch2. The last line is "submit_snapshot host test: N checks, F failures"
// and the exit code is 1 when F > 0.
//
// Fakes (one sentence each, checked against what Task 2 hands in):
// - MemRead: the SDK's ReadFn copies TranslatePhysical(address) for size bytes after a readability check and answers
//   false when a page is not readable; the fake copies from a byte vector indexed by address, answers false for the
//   one address it is told is unreadable, counts its calls and can flip one byte on the N-th read of one address.
// - WrittenFake: the SDK's GpuWrittenFn answers whether any page of [address, address + size) is GPU-written; the fake
//   answers whether the range overlaps its one region while switched on.
// - InvLog: the SDK's InvalidateFn marks [start, start + length) invalid; the fake records each call.

#include <rex/graphics/submit_snapshot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace ss = rex::graphics::submit_snapshot;

// Counting global allocation (group 18). Counts every operator new while g_count_allocs is set.
static std::atomic<bool> g_count_allocs{false};
static std::atomic<int> g_allocs{0};
void* operator new(std::size_t n) {
  if (g_count_allocs.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      std::fflush(stdout);                                             \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

// Progress marker per case, so a crash can be placed in the case that ran.
static void Case(const char* name) {
  std::printf("case %s\n", name);
  std::fflush(stdout);
}

static int Summary() {
  std::printf("submit_snapshot host test: %d checks, %d failures\n", g_checks, g_failures);
  std::fflush(stdout);
  return g_failures > 0 ? 1 : 0;
}

// Two byte patterns that differ at every address (different xor constants).
static uint8_t PatA(uint32_t a) { return uint8_t((a & 0xFF) ^ 0xA5); }
static uint8_t PatB(uint32_t a) { return uint8_t((a & 0xFF) ^ 0x3C); }

struct FakeMem {
  std::vector<uint8_t> bytes;
  uint32_t unreadable = 0xFFFFFFFFu;  // an address whose reads answer false
  int reads = 0;
  uint32_t flip_address = 0xFFFFFFFFu;  // flip bytes[flip_address] before the flip_on_read-th read of it
  int flip_on_read = 0;
  int flip_seen = 0;
  explicit FakeMem(uint32_t size) : bytes(size) {
    for (uint32_t a = 0; a < size; ++a) bytes[a] = PatA(a);
  }
};

static bool MemRead(void* ctx, uint32_t address, uint32_t size, uint8_t* dest) {
  auto* m = static_cast<FakeMem*>(ctx);
  ++m->reads;
  if (address == m->unreadable) return false;
  if (uint64_t(address) + size > m->bytes.size()) return false;
  if (address == m->flip_address && ++m->flip_seen == m->flip_on_read) m->bytes[address] ^= 0xFF;
  std::memcpy(dest, m->bytes.data() + address, size);
  return true;
}

struct WrittenFake {
  uint32_t address = 0, size = 0;
  bool on = false;
};

static bool WrittenFn(void* ctx, uint32_t address, uint32_t size) {
  auto* w = static_cast<WrittenFake*>(ctx);
  if (!w->on) return false;
  return uint64_t(address) < uint64_t(w->address) + w->size && uint64_t(w->address) < uint64_t(address) + size;
}

static bool NeverWritten(void*, uint32_t, uint32_t) { return false; }

struct InvLog {
  int n = 0;
  uint32_t start[16] = {};
  uint32_t length[16] = {};
};

static void InvFn(void* ctx, uint32_t start, uint32_t length) {
  auto* l = static_cast<InvLog*>(ctx);
  if (l->n < 16) {
    l->start[l->n] = start;
    l->length[l->n] = length;
  }
  ++l->n;
}

static void NoInv(void*, uint32_t, uint32_t) {}

// A boundary with an invalidate log and no GPU-written pages.
static void Boundary(ss::State& s, uint32_t ordinal, bool on, InvLog* inv) {
  s.GpuFrameBoundary(ordinal, on, InvFn, inv, NeverWritten, nullptr, 12);
}

// The learning signature: first seen under select 0x1 (pass 0) with no pages, then under 0x2 (pass 1) with a page.
// The frame's first select must be 0x1 for this to hold; every caller starts its frames with it.
static void LearnL(ss::Learner& l, uint32_t address, uint32_t size) {
  l.NoteRequest(address, size, 0x1, 0);
  l.NoteRequest(address, size, 0x2, 1);
}
static void LearnS(ss::State& s, uint32_t address, uint32_t size) {
  s.GpuNoteRequest(address, size, 0x1, 0);
  s.GpuNoteRequest(address, size, 0x2, 1);
}

static bool HasRange(std::span<const ss::Range> list, uint32_t address, uint32_t size) {
  for (const ss::Range& r : list) {
    if (r.address == address && r.size == size) return true;
  }
  return false;
}

// One numeric field of StatsLine(); UINT64_MAX when absent.
static uint64_t StatField(const ss::State& s, const char* name) {
  std::string line = s.StatsLine();
  std::string key = std::string(" ") + name + "=";
  size_t p = line.find(key);
  if (p == std::string::npos) return UINT64_MAX;
  p += key.size();
  if (p >= line.size() || line[p] < '0' || line[p] > '9') return UINT64_MAX;
  uint64_t v = 0;
  while (p < line.size() && line[p] >= '0' && line[p] <= '9') v = v * 10 + uint64_t(line[p++] - '0');
  return v;
}

// The token after " name=" in StatsLine(), up to the next space or the end; "<absent>" when the key is missing.
static std::string StatText(const ss::State& s, const char* name) {
  std::string line = s.StatsLine();
  std::string key = std::string(" ") + name + "=";
  size_t p = line.find(key);
  if (p == std::string::npos) return "<absent>";
  p += key.size();
  size_t e = line.find(' ', p);
  return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// A full list (the count cap): BeginFrame(frame), then kMaxRanges ranges of 16 bytes with 16-byte gaps from 0x10000
// (index i = 0x10000 + i * 0x20, the last ending at 0x10000 + kMaxRanges * 0x20), all last active at `frame`;
// kMaxRanges * 16 bytes in all (8192 at 512), far under the byte cap, so the count cap is the one that binds.
static void FullList(ss::Learner& l, uint32_t frame) {
  l.BeginFrame(frame);
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnL(l, 0x10000 + i * 0x20, 16);
  CHECK(l.ranges().size() == ss::kMaxRanges);
}

// The candidate the cap-evict cases refuse: past every FullList range.
static constexpr ss::Range kX{0x30000, 16};

// A guest window [base, base + size) with 4096 canary bytes on both sides.
// Live(): the live guest bytes the upload was copied from (Task 2: the memcpy's source, passed as `live`), in a
// buffer of their own as in the SDK. Groups 1-21 treat dest's bytes before the splice as live memory, so Live()
// copies them, at the call, into that buffer (sized here: no allocation in group 18). Group 22 sets live apart.
struct Window {
  uint32_t base, size;
  std::vector<uint8_t> buf;
  std::vector<uint8_t> live_buf;
  static constexpr uint32_t kCanary = 4096;
  Window(uint32_t b, uint32_t n) : base(b), size(n), buf(n + 2 * kCanary, 0xCC), live_buf(n) {
    for (uint32_t i = 0; i < n; ++i) dest()[i] = PatB(b + i);
  }
  uint8_t* dest() { return buf.data() + kCanary; }
  const uint8_t* Live() {
    std::memcpy(live_buf.data(), dest(), size);
    return live_buf.data();
  }
  uint8_t at(uint32_t address) { return dest()[address - base]; }
  void FillFrom(const FakeMem& m) {
    for (uint32_t i = 0; i < size; ++i) dest()[i] = m.bytes[base + i];
  }
  void FillB(uint32_t address, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dest()[address - base + i] = PatB(address + i);
  }
  bool CanariesIntact() const {
    for (uint32_t i = 0; i < kCanary; ++i) {
      if (buf[i] != 0xCC || buf[kCanary + size + i] != 0xCC) return false;
    }
    return true;
  }
};

// Bytes of [address, address + n) in w equal pattern A (true) or pattern B (false) at every address.
static bool AllPat(Window& w, uint32_t address, uint32_t n, bool a) {
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t want = a ? PatA(address + i) : PatB(address + i);
    if (w.at(address + i) != want) return false;
  }
  return true;
}

// 1. Only the learned ranges' bytes come from the copy; the rest of the page stays live.
static void TestSpliceTwoRanges() {
  Case("splice_two_ranges_and_other_data");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  LearnS(*s, 0x1100, 384);
  LearnS(*s, 0x1800, 768);
  ss::CopyResult c = s->GuestSwap(7, MemRead, &mem, false);
  CHECK(c.copied == 2 && c.bytes == 384 + 768);
  Boundary(*s, 6, true, &inv);  // frame 7
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
  CHECK(AllPat(w, 0x1100, 384, true));
  CHECK(AllPat(w, 0x1800, 768, true));
  CHECK(AllPat(w, 0x1000, 0x100, false));
  CHECK(AllPat(w, 0x1100 + 384, 0x1800 - (0x1100 + 384), false));
  CHECK(AllPat(w, 0x1800 + 768, 0x2000 - (0x1800 + 768), false));
  CHECK(w.CanariesIntact());
}

// 2. A range across a page edge is clipped to each splice window.
static void TestRangeAcrossPageEdge() {
  Case("range_across_page_edge");
  ss::Ring ring;
  FakeMem mem(0x10000);
  const ss::Range r{0x1F00, 512};
  ss::CopyResult c = ring.Copy(1, {&r, 1}, MemRead, &mem, false);
  CHECK(c.copied == 1);
  std::vector<ss::Range> diff;
  Window w1(0x1000, 4096);
  ss::SpliceResult a = ring.Splice(1, 0x1000, 4096, w1.dest(), w1.Live(), {&r, 1}, &diff);
  CHECK(a.missing == ss::Missing::kNone && a.spliced == 1);
  CHECK(AllPat(w1, 0x1F00, 0x100, true));
  CHECK(AllPat(w1, 0x1000, 0xF00, false));
  CHECK(w1.CanariesIntact());
  Window w2(0x2000, 4096);
  ss::SpliceResult b = ring.Splice(1, 0x2000, 4096, w2.dest(), w2.Live(), {&r, 1}, &diff);
  CHECK(b.missing == ss::Missing::kNone && b.spliced == 1);
  CHECK(AllPat(w2, 0x2000, 0x100, true));
  CHECK(AllPat(w2, 0x2100, 0xF00, false));
  CHECK(w2.CanariesIntact());
}

// 3. Missing copies: the slot holds a newer ordinal (reused) or an older one (not yet); dest untouched in both.
static void TestCopyMissingReasons() {
  Case("copy_missing_reasons");
  ss::Ring ring;
  FakeMem mem(0x10000);
  const ss::Range r{0x1100, 64};
  ring.Copy(7, {&r, 1}, MemRead, &mem, false);  // slot 3
  std::vector<ss::Range> diff;
  Window w(0x1000, 4096);
  std::vector<uint8_t> saved(w.dest(), w.dest() + 4096);
  ss::SpliceResult a = ring.Splice(3, 0x1000, 4096, w.dest(), w.Live(), {&r, 1}, &diff);
  CHECK(a.missing == ss::Missing::kReused);
  CHECK(std::memcmp(saved.data(), w.dest(), 4096) == 0);
  ss::SpliceResult b = ring.Splice(11, 0x1000, 4096, w.dest(), w.Live(), {&r, 1}, &diff);
  CHECK(b.missing == ss::Missing::kNotYet);
  CHECK(std::memcmp(saved.data(), w.dest(), 4096) == 0);
  // Never written: slot 1.
  ss::SpliceResult n = ring.Splice(5, 0x1000, 4096, w.dest(), w.Live(), {&r, 1}, &diff);
  CHECK(n.missing == ss::Missing::kNotYet);
  CHECK(std::memcmp(saved.data(), w.dest(), 4096) == 0);
  // The slot's own frame splices (positive companion).
  ss::SpliceResult k = ring.Splice(7, 0x1000, 4096, w.dest(), w.Live(), {&r, 1}, &diff);
  CHECK(k.missing == ss::Missing::kNone && k.spliced == 1 && k.differing == 1);
  CHECK(diff.size() == 1 && diff[0].address == 0x1100 && diff[0].size == 64);
  CHECK(AllPat(w, 0x1100, 64, true));
}

// 4. Equal bytes keep no span; differing bytes keep their pages until the next boundary, which empties the list.
static void TestKeptSpans() {
  Case("equal_bytes_not_kept_differing_kept");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  LearnS(*s, 0x1100, 384);      // page 1
  LearnS(*s, 0x3800, 768);      // page 3
  s->GuestSwap(2, MemRead, &mem, false);
  Window w(0x1000, 0x4000);
  w.FillFrom(mem);  // copy == live
  s->GpuSplice(0x1000, 0x4000, w.dest(), w.Live(), 12);
  CHECK(StatField(*s, "spliced") == 1);
  CHECK(StatField(*s, "differing") == 0);
  inv = InvLog{};
  Boundary(*s, 2, true, &inv);  // frame 3
  CHECK(inv.n == 0);
  // One range differs.
  s->GuestSwap(3, MemRead, &mem, false);
  w.FillFrom(mem);
  w.FillB(0x3800, 768);
  s->GpuSplice(0x1000, 0x4000, w.dest(), w.Live(), 12);
  CHECK(StatField(*s, "spliced") == 2);
  CHECK(StatField(*s, "differing") == 1);
  inv = InvLog{};
  Boundary(*s, 3, true, &inv);  // frame 4
  CHECK(inv.n == 1 && inv.start[0] == 0x3000 && inv.length[0] == 0x1000);
  inv = InvLog{};
  Boundary(*s, 3, true, &inv);  // frame 4 again: the kept list is empty now
  CHECK(inv.n == 0);
  // Two differing ranges on touching pages 1 and 2: one merged span.
  LearnS(*s, 0x2200, 64);  // page 2
  s->GuestSwap(4, MemRead, &mem, false);
  w.FillFrom(mem);
  w.FillB(0x1100, 384);
  w.FillB(0x2200, 64);
  s->GpuSplice(0x1000, 0x4000, w.dest(), w.Live(), 12);
  CHECK(StatField(*s, "differing") == 2);
  inv = InvLog{};
  Boundary(*s, 4, true, &inv);  // frame 5
  CHECK(inv.n == 1 && inv.start[0] == 0x1000 && inv.length[0] == 0x2000);
  // A span is clipped to the splice window: a window holding only page 2 of the range {0x1F00, 512} keeps page 2.
  LearnS(*s, 0x1F00, 512);  // merges nothing: 0x1F00..0x2100 overlaps neither 0x1100..0x1280 nor 0x2200..0x2240
  s->GuestSwap(5, MemRead, &mem, false);
  Window w2(0x2000, 0x1000);  // pattern B: differs from the copy
  s->GpuSplice(0x2000, 0x1000, w2.dest(), w2.Live(), 12);
  inv = InvLog{};
  Boundary(*s, 5, true, &inv);
  CHECK(inv.n == 1 && inv.start[0] == 0x2000 && inv.length[0] == 0x1000);
}

// 5. Page spans for the prefetch rule: whole pages covering each range, end page rounded up.
static void TestLearnedPageSpans() {
  Case("learned_page_spans_for_prefetch");
  ss::Learner l;
  l.BeginFrame(1);
  LearnL(l, 0x1F00, 512);
  LearnL(l, 0x5000, 16);
  const auto& spans = l.LearnedPageSpans(12);
  CHECK(spans.size() == 2);
  CHECK(spans.size() == 2 && spans[0].first == 1 && spans[0].second == 2);
  CHECK(spans.size() == 2 && spans[1].first == 5 && spans[1].second == 1);
  bool page3 = false;
  for (const auto& sp : spans) {
    if (3 >= sp.first && 3 < sp.first + sp.second) page3 = true;
  }
  CHECK(!page3);
  // The State forwards its learner's spans.
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);
  LearnS(*s, 0x1F00, 512);
  const auto& st = s->LearnedPageSpans(12);
  CHECK(st.size() == 1 && st[0].first == 1 && st[0].second == 2);
}

// 6. The signature: first seen in a lower pass of the frame, then re-requested with pages in a higher one.
static void TestLearningSignature() {
  Case("learning_signature");
  const uint32_t A = 0x4000, B = 0x6000;
  {
    ss::Learner l;
    l.BeginFrame(1);
    l.NoteRequest(A, 512, 0x1, 0);
    l.NoteRequest(A, 512, 0x2, 1);
    CHECK(l.ranges().size() == 1 && HasRange(l.ranges(), A, 512));
    uint64_t g = l.generation();
    CHECK(g > 0);
    l.NoteRequest(A, 512, 0x2, 1);  // the same range again: no change
    CHECK(l.generation() == g);
    l.NoteRequest(A, 256, 0x2, 1);  // a range inside it: no change
    CHECK(l.generation() == g && l.ranges().size() == 1);
  }
  {
    ss::Learner l;  // same pass re-upload
    l.BeginFrame(1);
    l.NoteRequest(A, 512, 0x1, 0);
    l.NoteRequest(A, 512, 0x1, 1);
    CHECK(l.ranges().empty());
    CHECK(l.generation() == 0);
  }
  {
    ss::Learner l;  // later pass without pages
    l.BeginFrame(1);
    l.NoteRequest(A, 512, 0x1, 0);
    l.NoteRequest(A, 512, 0x2, 0);
    CHECK(l.ranges().empty());
    CHECK(l.generation() == 0);
  }
  {
    ss::Learner l;  // D9 order (A4): pass index = first-seen order of the frame's selects
    l.BeginFrame(1);
    l.NoteRequest(B, 16, 0x1, 0);   // 0x1 = pass 0
    l.NoteRequest(A, 512, 0x2, 0);  // 0x2 = pass 1; A first seen in pass 1
    l.NoteRequest(A, 512, 0x1, 1);  // pass 0 < A's first pass: not learned
    CHECK(l.ranges().empty());
    CHECK(l.generation() == 0);
    l.NoteRequest(B, 16, 0x2, 1);  // B first seen in pass 0, now pass 1 with pages: learned (control)
    CHECK(l.ranges().size() == 1 && HasRange(l.ranges(), B, 16));
  }
  {
    ss::Learner l;  // a new frame forgets the notes
    l.BeginFrame(1);
    l.NoteRequest(A, 512, 0x1, 0);  // A first seen in pass 0 of frame 1
    l.BeginFrame(2);
    l.NoteRequest(B, 16, 0x1, 0);   // 0x1 = pass 0 of frame 2
    l.NoteRequest(A, 512, 0x2, 1);  // A first seen in pass 1 of frame 2: not learned
    CHECK(l.ranges().empty());
  }
}

// 7. Limits at both edges: kMaxRanges ranges, 64 KB per range, 512 KB in all.
static void TestLimits() {
  Case("limits_both_edges");
  {
    ss::Learner l;
    l.BeginFrame(1);
    std::vector<ss::Range> candidates;
    for (uint32_t i = 0; i < ss::kMaxRanges + 1; ++i) candidates.push_back({0x10000 + i * 0x20, 16});  // 16-byte gaps
    CHECK(candidates.size() == ss::kMaxRanges + 1);
    for (const ss::Range& c : candidates) LearnL(l, c.address, c.size);
    CHECK(l.ranges().size() == ss::kMaxRanges);
    CHECK(!HasRange(l.ranges(), candidates[ss::kMaxRanges].address, 16));
  }
  {
    ss::Learner l;
    l.BeginFrame(1);
    LearnL(l, 0x100000, 65536);
    LearnL(l, 0x200000, 65537);
    CHECK(HasRange(l.ranges(), 0x100000, 65536));
    CHECK(!HasRange(l.ranges(), 0x200000, 65537));
    CHECK(l.ranges().size() == 1);
  }
  {
    ss::Learner l;
    l.BeginFrame(1);
    for (uint32_t i = 0; i < 8; ++i) LearnL(l, 0x100000 + i * 0x20000, 65536);  // 128 KB stride: gaps
    uint64_t sum = 0;
    for (const ss::Range& r : l.ranges()) sum += r.size;
    CHECK(l.ranges().size() == 8);
    CHECK(sum == 7 * 65536 + 65536 && sum == 524288);
    LearnL(l, 0x300000, 1);  // one more byte, a separate range
    CHECK(l.ranges().size() == 8);
    CHECK(!HasRange(l.ranges(), 0x300000, 1));
  }
  {
    // The refusals are counted (A16).
    auto s = std::make_unique<ss::State>();
    InvLog inv;
    Boundary(*s, 1, true, &inv);
    for (uint32_t i = 0; i < ss::kMaxRanges + 1; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
    CHECK(StatField(*s, "ranges_now") == ss::kMaxRanges);
    CHECK(StatField(*s, "rejected_cap") == 1);
    LearnS(*s, 0x200000, 65537);
    CHECK(StatField(*s, "rejected_size") == 1);
    CHECK(StatField(*s, "rejected_cap") == 1);
  }
}

// 43. The count cap is 512 (ledger SS83: the Playground refused about 7,000 candidates a minute at 256 with the bytes at
// 178K of 512K). 512 ranges of 16 bytes (8192 bytes, far under the byte cap) are all held; the 513th distinct range is
// refused by the count cap. The value is pinned here as a literal, not as kMaxRanges.
static void TestCountCapIs512() {
  Case("count_cap_is_512");
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < 512; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
  CHECK(StatField(*s, "ranges_now") == 512);
  CHECK(StatField(*s, "ranges_bytes") == 8192);
  CHECK(StatField(*s, "rejected_cap") == 0);
  CHECK(StatText(*s, "refused_first") == "-");
  // The full list goes through the Ring whole (review 4b M1): GuestSwap -> Ring::Copy copies all 512 ranges, not a
  // cut at 256. The last range ends at 0x10000 + 511 * 0x20 + 16 = 0x13FF0, inside the 0x14000-byte FakeMem.
  FakeMem mem(0x14000);
  ss::CopyResult c = s->GuestSwap(2, MemRead, &mem, false);
  CHECK(c.copied == 512);
  CHECK(c.bytes == 8192 && c.skipped_unreadable == 0);
  LearnS(*s, 0x30000, 16);  // the 513th distinct range: count 513, bytes 8208
  CHECK(StatField(*s, "rejected_cap") == 1);
  CHECK(StatText(*s, "refused_first") == "0x30000:16:count");
  CHECK(StatField(*s, "ranges_now") == 512);
}

// 24. Cap eviction (spec 2026-11-23 cap-evict, G1/G2): a refused candidate waits for the next BeginFrame, which
// evicts the longest-idle range idle >= kEvictIdleFrames (8), ties to the lowest index; never mid-frame.
// Excluded wrong versions: evict the first index whatever its age (boundary 8), ">" for ">=" (boundary 9), a
// threshold of 7 (boundary 8), eviction inside TryLearn (the frame-9 mid-frame check).
static void TestEvictIdleEdge() {
  Case("evict_idle_edge");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(7);
  uint64_t g7 = l.generation();
  LearnL(l, kX.address, kX.size);  // refused: count kMaxRanges + 1
  CHECK(!HasRange(l.ranges(), kX.address, kX.size));
  CHECK(l.ranges().size() == ss::kMaxRanges);
  CHECK(l.generation() == g7);  // the refusal changes nothing mid-frame
  l.BeginFrame(8);  // every range idle 7 < 8: X dropped
  CHECK(!HasRange(l.ranges(), kX.address, kX.size) && HasRange(l.ranges(), 0x10000, 16));
  CHECK(l.evicted() == 0);
  l.NoteChanged({0x10000, 16});  // index 0 last active 8
  LearnL(l, kX.address, kX.size);  // refused again
  uint64_t g8 = l.generation();
  l.BeginFrame(9);  // index 0 idle 1, indices 1..kMaxRanges-1 idle 8 (eligible): index 1 is the victim
  CHECK(HasRange(l.ranges(), kX.address, kX.size));
  CHECK(l.generation() == g8 + 1);  // one step for the boundary's eviction and insert together (R2')
  CHECK(!HasRange(l.ranges(), 0x10020, 16) && HasRange(l.ranges(), 0x10000, 16));
  CHECK(l.ranges().size() == ss::kMaxRanges);
  uint64_t g9 = l.generation();
  LearnL(l, 0x30100, 16);  // refused: count kMaxRanges + 1; nothing is evicted mid-frame
  CHECK(!HasRange(l.ranges(), 0x30100, 16) && HasRange(l.ranges(), 0x10040, 16) && l.generation() == g9 &&
        l.ranges().size() == ss::kMaxRanges);
  l.BeginFrame(10);  // X idle 1 (protected), index 0 idle 2, 0x10040 (now index 1) idle 9: the victim
  CHECK(HasRange(l.ranges(), kX.address, kX.size));
  CHECK(!HasRange(l.ranges(), 0x10040, 16));
  CHECK(HasRange(l.ranges(), 0x30100, 16));
  CHECK(l.evicted() == 2);
}

// 25. The victim is the LONGEST idle range, not the lowest-index eligible one: index 0 idle 11 (eligible) loses to
// index 1 idle 12.
static void TestEvictLongestIdle() {
  Case("evict_longest_idle");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(2);
  l.NoteChanged({0x10000, 16});  // index 0 last active 2
  l.BeginFrame(12);
  LearnL(l, kX.address, kX.size);  // refused
  CHECK(!HasRange(l.ranges(), kX.address, kX.size));
  l.BeginFrame(13);
  CHECK(HasRange(l.ranges(), 0x10000, 16) && !HasRange(l.ranges(), 0x10020, 16) &&
        HasRange(l.ranges(), kX.address, kX.size));
}

// 26. Nothing idle: every range changed each frame (idle 1 at each boundary), the candidate is refused at every
// boundary and nothing is evicted. Companion: once the ranges go quiet (last active 20, idle 9 at 29), it is admitted.
static void TestNothingIdleRefuses() {
  Case("nothing_idle_refuses");
  ss::Learner l;
  FullList(l, 1);
  bool absent = true;
  for (uint32_t f = 2; f <= 20; ++f) {
    l.BeginFrame(f);
    if (HasRange(l.ranges(), kX.address, kX.size)) absent = false;
    l.NoteChanged({0x10000, ss::kMaxRanges * 0x20});  // every range last active f
    LearnL(l, kX.address, kX.size);
  }
  l.BeginFrame(21);
  CHECK(absent && !HasRange(l.ranges(), kX.address, kX.size));
  CHECK(l.ranges().size() == ss::kMaxRanges);
  CHECK(l.evicted() == 0);
  for (uint32_t f = 22; f <= 28; ++f) l.BeginFrame(f);  // no NoteChanged: every range last active 20
  LearnL(l, kX.address, kX.size);                        // frame 28, refused
  l.BeginFrame(29);                                      // every range idle 9
  CHECK(HasRange(l.ranges(), kX.address, kX.size));
  CHECK(l.evicted() == 1);
}

// 27. At most kMaxPending (16) candidates per boundary, deduplicated: C0 refused twice takes one slot, so C0..C15
// are admitted and C16..C19 are not. Without the dedup C15 is missing; without the bound C16 is present.
static void TestPendingBoundAndDedup() {
  Case("pending_bound_and_dedup");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(20);
  const uint32_t n = 20;
  CHECK(n > ss::kMaxPending);
  LearnL(l, 0x40000, 16);  // C0
  for (uint32_t k = 0; k < n; ++k) LearnL(l, 0x40000 + k * 0x20, 16);  // C0 again, C1..C19
  CHECK(l.ranges().size() == ss::kMaxRanges);
  l.BeginFrame(21);  // every fill range idle 20
  bool admitted = true, refused = true, victims = true;
  for (uint32_t k = 0; k < 16; ++k) admitted = admitted && HasRange(l.ranges(), 0x40000 + k * 0x20, 16);
  for (uint32_t k = 16; k < n; ++k) refused = refused && !HasRange(l.ranges(), 0x40000 + k * 0x20, 16);
  for (uint32_t i = 0; i < 16; ++i) victims = victims && !HasRange(l.ranges(), 0x10000 + i * 0x20, 16);
  CHECK(admitted);
  CHECK(HasRange(l.ranges(), 0x40000 + 15 * 0x20, 16));  // C15: the slot the dedup saved
  CHECK(refused);
  CHECK(!HasRange(l.ranges(), 0x40000 + 16 * 0x20, 16));  // C16: past the bound
  CHECK(l.ranges().size() == ss::kMaxRanges);
  CHECK(l.evicted() == 16);
  CHECK(victims && HasRange(l.ranges(), 0x10000 + 16 * 0x20, 16));  // the 16 lowest fill ranges, and only those
}

// 28. Bytes cap: the ranges the candidate overlaps merge into it and are never victims. Merged {0x100000, 98304}
// would make 557056 bytes; evicting 0x120000 (the lowest-index range outside the overlap) leaves 491520.
static void TestEvictOverlapNotVictim() {
  Case("evict_overlap_not_victim");
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 8; ++i) LearnL(l, 0x100000 + i * 0x20000, 65536);
  uint64_t sum = 0;
  for (const ss::Range& r : l.ranges()) sum += r.size;
  CHECK(l.ranges().size() == 8 && sum == 524288);
  l.BeginFrame(20);
  LearnL(l, 0x108000, 65536);  // refused: bytes 524288 - 65536 + 98304 = 557056
  CHECK(!HasRange(l.ranges(), 0x100000, 98304) && l.ranges().size() == 8);
  l.BeginFrame(21);
  CHECK(HasRange(l.ranges(), 0x100000, 98304));
  CHECK(!HasRange(l.ranges(), 0x120000, 65536));
  CHECK(l.ranges().size() == 7);
  CHECK(!HasRange(l.ranges(), 0x108000, 65536));  // the wrong version evicts the overlapped range 0
}

// 29. Bytes cap, several victims for one candidate: 16 x 32768 = 524288; {0x400000, 65536} needs two evictions.
static void TestBytesCapEvictsSeveral() {
  Case("bytes_cap_evicts_several");
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 16; ++i) LearnL(l, 0x200000 + i * 0x10000, 32768);
  uint64_t sum = 0;
  for (const ss::Range& r : l.ranges()) sum += r.size;
  CHECK(l.ranges().size() == 16 && sum == 524288);
  l.BeginFrame(20);
  LearnL(l, 0x400000, 65536);  // refused: bytes 589824
  l.BeginFrame(21);
  CHECK(HasRange(l.ranges(), 0x400000, 65536));
  CHECK(l.ranges().size() == 15);
  CHECK(!HasRange(l.ranges(), 0x200000, 32768) && !HasRange(l.ranges(), 0x210000, 32768));
  CHECK(HasRange(l.ranges(), 0x220000, 32768));
  CHECK(l.evicted() == 2);
}

// 30. A pending candidate on a page banned before its boundary is dropped at admission. In State a ban at boundary
// N+1 already existed in frame N (State runs BanOverlapping right after BeginFrame), so this order is reachable only
// by direct Learner callers. Companion: a second pending candidate is admitted at the same boundary.
static void TestBanBeforeAdmission() {
  Case("ban_before_admission");
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < ss::kMaxRanges - 1; ++i) LearnL(l, 0x10000 + i * 0x20, 16);
  LearnL(l, 0x9000, 16);  // R, index 0
  CHECK(l.ranges().size() == ss::kMaxRanges);
  l.BeginFrame(20);
  LearnL(l, 0x9100, 16);   // refused (count); R's 4 KB page
  LearnL(l, 0x20000, 16);  // refused (count)
  WrittenFake wf{0x9000, 4096, true};
  l.BanOverlapping(WrittenFn, &wf, 12);  // R banned and dropped
  CHECK(!HasRange(l.ranges(), 0x9000, 16) && l.ranges().size() == ss::kMaxRanges - 1);
  uint64_t g = l.generation();
  l.BeginFrame(21);
  CHECK(!HasRange(l.ranges(), 0x9100, 16));
  CHECK(HasRange(l.ranges(), 0x20000, 16));
  CHECK(l.generation() == g + 1);  // an insert with no eviction moves the generation too (R2'): State publishes on it
  CHECK(l.ranges().size() == ss::kMaxRanges);
  CHECK(l.evicted() == 0);
}

// 31. R3-pre (ruling SS77, replacing R3' "evictions made before a drop stand"): a candidate that cannot fit even with
// every eligible victim gone is dropped with NO eviction. Only index 0 is idle >= 8 (idle 20; the others idle 2);
// evicting it would leave 491520 + 65536 = 557056 > 524288, so nothing is evicted and the list and its generation
// stay as they were. The excluded wrong version is R3': index 0 evicted, 15 ranges, the generation moved.
static void TestPartialEvictThenDrop() {
  Case("partial_evict_then_drop");
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 16; ++i) LearnL(l, 0x200000 + i * 0x10000, 32768);
  CHECK(l.ranges().size() == 16);
  l.BeginFrame(19);
  l.NoteChanged({0x210000, 15 * 0x10000});  // indices 1..15 last active 19
  l.BeginFrame(20);
  LearnL(l, 0x400000, 65536);  // refused (bytes)
  CHECK(!HasRange(l.ranges(), 0x400000, 65536));
  uint64_t g = l.generation();
  l.BeginFrame(21);
  CHECK(!HasRange(l.ranges(), 0x400000, 65536));
  CHECK(HasRange(l.ranges(), 0x200000, 32768));
  CHECK(l.ranges().size() == 16);
  CHECK(l.evicted() == 0);
  CHECK(l.generation() == g);
}

// 37. Churn is bounded (spec G2; ruling SS77): a candidate larger than every eligible victim together never evicts.
// 8 ranges of 65000 bytes that change every frame (520000) + kMaxRanges - 8 ranges of 8 bytes requested every frame
// that never change (504 x 8 = 4032 at 512) = 524032 bytes, kMaxRanges ranges: the list is full by count and within
// 256 bytes of the byte cap. A 65536-byte candidate C is requested every frame (count kMaxRanges + 1, bytes 589568):
// 524032 + 65536 - 4032 = 585536 > 524288 even with every small range evicted. Under R3' every 8th boundary wiped the
// small ranges (at 256: 2976 evictions in 100 boundaries, C never admitted; review I1's probe).
static void TestFutileEvictionDrops() {
  Case("futile_eviction_drops");
  constexpr uint32_t kSmall = ss::kMaxRanges - 8;
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 8; ++i) LearnL(l, 0x1000000 + i * 0x20000, 65000);
  for (uint32_t j = 0; j < kSmall; ++j) LearnL(l, 0x100000 + j * 0x20, 8);
  uint64_t sum0 = 0;
  for (const ss::Range& r : l.ranges()) sum0 += r.size;
  CHECK(l.ranges().size() == ss::kMaxRanges && sum0 == 8 * 65000 + kSmall * 8);
  CHECK(sum0 + 65536 - kSmall * 8 > ss::kMaxTotalBytes);  // the precondition: C cannot fit even with every victim gone
  bool c_ever = false, refused_every_frame = true;
  size_t min_size = ss::kMaxRanges;
  for (uint32_t f = 2; f <= 101; ++f) {
    l.BeginFrame(f);
    min_size = std::min(min_size, l.ranges().size());
    if (HasRange(l.ranges(), 0x2000000, 65536)) c_ever = true;
    l.NoteChanged({0x1000000, 8 * 0x20000});  // the 8 big ranges change every frame
    for (uint32_t j = 0; j < kSmall; ++j) LearnL(l, 0x100000 + j * 0x20, 8);
    LearnL(l, 0x2000000, 65536);
    if (HasRange(l.ranges(), 0x2000000, 65536)) refused_every_frame = false;
  }
  CHECK(refused_every_frame);
  CHECK(!c_ever);
  CHECK(l.evicted() == 0);
  CHECK(min_size == ss::kMaxRanges);  // no boundary removed a range
}

// 42. R3-pre at its byte edge: a candidate that fits EXACTLY at kMaxTotalBytes once every eligible range is gone is
// admitted (the admission rule is bytes_after <= kMaxTotalBytes; R3-pre drops only past it). 16 x 32768 = 524288;
// indices 2..15 changed at 19, so only indices 0 and 1 are idle >= 8 at 21: 524288 + 65536 - 2 x 32768 = 524288.
// The excluded wrong version drops at ">=" (nothing evicted, the candidate absent). The count edge (exactly kMaxRanges after
// the only eligible victim goes) is group 39's C_a.
static void TestPreCheckAtByteEdge() {
  Case("pre_check_at_byte_edge");
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 16; ++i) LearnL(l, 0x200000 + i * 0x10000, 32768);
  uint64_t sum0 = 0;
  for (const ss::Range& r : l.ranges()) sum0 += r.size;
  CHECK(l.ranges().size() == 16 && sum0 == 524288);
  l.BeginFrame(19);
  l.NoteChanged({0x220000, 14 * 0x10000});  // indices 2..15 last active 19
  l.BeginFrame(20);
  LearnL(l, 0x400000, 65536);  // refused (bytes 589824)
  CHECK(!HasRange(l.ranges(), 0x400000, 65536));
  l.BeginFrame(21);
  CHECK(HasRange(l.ranges(), 0x400000, 65536));
  CHECK(!HasRange(l.ranges(), 0x200000, 32768) && !HasRange(l.ranges(), 0x210000, 32768));
  CHECK(HasRange(l.ranges(), 0x220000, 32768));
  CHECK(l.ranges().size() == 15);
  CHECK(l.evicted() == 2);
  uint64_t sum1 = 0;
  for (const ss::Range& r : l.ranges()) sum1 += r.size;
  CHECK(sum1 == ss::kMaxTotalBytes);  // exactly at the cap
}

// 44. R3-pre counts only victims OUTSIDE the candidate's overlap set (re-review m-rr1, mutant mB). A = {0x200000, 16}
// idle and overlapped by c; B = {0x300000, 16} idle, not overlapped; fillers active every frame, 14 x 32768 + 8 =
// 458760 bytes. c = {0x1F8000, 65536} merges A: bytes_after = 458760 + 32 - 16 + 65536 = 524312 > 524288 (refused,
// bytes; count 17, far under the count cap). Eligible = B only: 524312 - 16 = 524296 > 524288, so c is dropped with no
// eviction. The excluded wrong version also counts A (524312 - 32 = 524280 passes), then evicts B and still drops c.
static void TestPreCheckSkipsOverlap() {
  Case("pre_check_skips_overlap");
  ss::Learner l;
  l.BeginFrame(1);
  LearnL(l, 0x200000, 16);  // A
  LearnL(l, 0x300000, 16);  // B
  for (uint32_t i = 0; i < 14; ++i) LearnL(l, 0x400000 + i * 0x10000, 32768);
  LearnL(l, 0x400000 + 14 * 0x10000, 8);
  uint64_t sum0 = 0;
  for (const ss::Range& r : l.ranges()) sum0 += r.size;
  CHECK(l.ranges().size() == 17 && sum0 == 458760 + 32);
  l.BeginFrame(19);
  l.NoteChanged({0x400000, 15 * 0x10000});  // the fillers last active 19
  l.BeginFrame(20);
  LearnL(l, 0x1F8000, 65536);  // refused (bytes 524312)
  CHECK(!HasRange(l.ranges(), 0x1F8000, 65536) && l.ranges().size() == 17);
  uint64_t g = l.generation();
  l.BeginFrame(21);  // A and B idle 20, the fillers idle 2
  CHECK(!HasRange(l.ranges(), 0x1F8000, 65536));
  CHECK(HasRange(l.ranges(), 0x300000, 16));  // B not evicted
  CHECK(HasRange(l.ranges(), 0x200000, 16));
  CHECK(l.ranges().size() == 17);
  CHECK(l.evicted() == 0);
  CHECK(l.generation() == g);
}

// 45. R3-pre runs for EVERY pending candidate, not only the first (re-review m-rr1, mutant mC). The list of group 37
// (8 x 65000 active every frame + kMaxRanges - 8 ranges of 8 bytes, last active 1): kMaxRanges ranges, 524032 bytes.
// Frame 8 refuses D = {0x30000000, 16} (count kMaxRanges + 1), then C = {0x2000000, 65536}. Boundary 9 (the small
// ranges idle 8): D passes its pre-check (count_after - n_elig = 9) and evicts one small range, the lowest index
// 0x100000, then fits (bytes 524040). C: bytes_after 524040 + 65536 = 589576, eligible 503 x 8 = 4024 (D is idle 0),
// 585552 > 524288, so C is dropped with no eviction: evicted 1. The excluded wrong version pre-checks only the first
// pending candidate: C wipes all 503 small ranges and is still dropped (evicted 504).
// A single boundary: in group 37's loop the small range D evicts is re-requested, refused and pended, and evicts
// another at each later boundary (legitimate churn), so the count there is not 1.
static void TestPreCheckEveryCandidate() {
  Case("pre_check_every_candidate");
  constexpr uint32_t kSmall = ss::kMaxRanges - 8;
  ss::Learner l;
  l.BeginFrame(1);
  for (uint32_t i = 0; i < 8; ++i) LearnL(l, 0x1000000 + i * 0x20000, 65000);
  for (uint32_t j = 0; j < kSmall; ++j) LearnL(l, 0x100000 + j * 0x20, 8);
  uint64_t sum0 = 0;
  for (const ss::Range& r : l.ranges()) sum0 += r.size;
  CHECK(l.ranges().size() == ss::kMaxRanges && sum0 == 8 * 65000 + kSmall * 8);
  for (uint32_t f = 2; f <= 8; ++f) {
    l.BeginFrame(f);
    l.NoteChanged({0x1000000, 8 * 0x20000});  // the 8 big ranges change every frame
  }
  LearnL(l, 0x30000000, 16);   // D, refused first (count)
  LearnL(l, 0x2000000, 65536);  // C, refused second
  CHECK(!HasRange(l.ranges(), 0x30000000, 16) && !HasRange(l.ranges(), 0x2000000, 65536));
  l.BeginFrame(9);
  CHECK(HasRange(l.ranges(), 0x30000000, 16));
  CHECK(!HasRange(l.ranges(), 0x100000, 8) && HasRange(l.ranges(), 0x100020, 8));
  CHECK(!HasRange(l.ranges(), 0x2000000, 65536));
  CHECK(l.evicted() == 1);
  CHECK(l.ranges().size() == ss::kMaxRanges);
  bool smalls_kept = true;
  for (uint32_t j = 1; j < kSmall; ++j) smalls_kept = smalls_kept && HasRange(l.ranges(), 0x100000 + j * 0x20, 8);
  CHECK(smalls_kept);
}

// 38. R2: aging runs BEFORE admission. Index 5 (0x100A0) last active 1, the others 115. Boundary 121: index 5 idle
// 120, not aged. X refused at 121. Boundary 122: index 5 idle 121 > 120 is aged first, so X fits with no eviction.
// The excluded wrong version admits first: it evicts index 5 for X (evicted 1).
static void TestR2AgingBeforeAdmission() {
  Case("r2_aging_before_admission");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(115);
  l.NoteChanged({0x10000, 5 * 0x20});
  l.NoteChanged({0x10000 + 6 * 0x20, (ss::kMaxRanges - 6) * 0x20});  // indices 6..kMaxRanges-1
  l.BeginFrame(121);
  CHECK(HasRange(l.ranges(), 0x100A0, 16) && l.ranges().size() == ss::kMaxRanges);
  LearnL(l, kX.address, kX.size);
  CHECK(!HasRange(l.ranges(), kX.address, kX.size));
  l.BeginFrame(122);
  CHECK(HasRange(l.ranges(), kX.address, kX.size));
  CHECK(!HasRange(l.ranges(), 0x100A0, 16));
  CHECK(l.ranges().size() == ss::kMaxRanges);
  CHECK(l.evicted() == 0);
}

// 39. R3: pending candidates are admitted in pending order (first refused, first admitted). Only index 0 is idle >= 8
// at boundary 21; C_a (refused first) takes it, C_b finds no victim (C_a is idle 0) and is dropped. The excluded
// wrong version admits the last-refused candidate first.
static void TestR3PendingOrder() {
  Case("r3_pending_order");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(19);
  l.NoteChanged({0x10020, (ss::kMaxRanges - 1) * 0x20});
  l.BeginFrame(20);
  LearnL(l, 0x30000, 16);
  LearnL(l, 0x30100, 16);
  l.BeginFrame(21);
  CHECK(HasRange(l.ranges(), 0x30000, 16));
  CHECK(!HasRange(l.ranges(), 0x30100, 16));
  CHECK(l.evicted() == 1 && l.ranges().size() == ss::kMaxRanges);
}

static bool PageListed(ss::State& s, uint32_t page) {
  for (const auto& [first, count] : s.LearnedPageSpans(12)) {
    if (page >= first && page < first + count) return true;
  }
  return false;
}

// 40. R4': a candidate pending at a frame_ == 0 boundary (ordinal 0, then latched off) survives it and is admitted at
// the next BeginFrame. The excluded wrong version clears pending at a frame_ == 0 boundary.
static void TestR4PendingSurvivesFrameZero() {
  Case("r4_pending_survives_frame_zero");
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
  LearnS(*s, kX.address, kX.size);
  CHECK(StatField(*s, "rejected_cap") == 1);
  CHECK(!PageListed(*s, 0x30));
  Boundary(*s, 0, true, &inv);   // frame 0
  Boundary(*s, 5, false, &inv);  // latched off: frame 0
  Boundary(*s, 20, true, &inv);  // frame 21: every fill range idle 19
  CHECK(PageListed(*s, 0x30));
  CHECK(StatField(*s, "evicted") == 1);
  CHECK(StatField(*s, "ranges_now") == ss::kMaxRanges);
}

// 41. The eviction idle expression across the frame-counter wrap (same form as aging). Fill at 0xFFFFFFF8; frame 3
// is idle 11, nothing aged; boundary 4 admits X by one eviction (idle 12 >= 8); at boundary 5 X (idle 1) is protected
// and a fill range (idle 13) is the victim.
static void TestIdleAtCounterWrap() {
  Case("idle_at_counter_wrap");
  ss::Learner l;
  FullList(l, 0xFFFFFFF8u);
  l.BeginFrame(3);
  CHECK(l.ranges().size() == ss::kMaxRanges);  // idle 11 <= 120: not aged
  LearnL(l, kX.address, kX.size);
  l.BeginFrame(4);  // idle 12 >= 8: X admitted by one eviction
  CHECK(HasRange(l.ranges(), kX.address, kX.size) && l.evicted() == 1);
  LearnL(l, 0x30100, 16);
  l.BeginFrame(5);  // X idle 1 (protected); a fill range idle 13 is the victim
  CHECK(HasRange(l.ranges(), kX.address, kX.size) && HasRange(l.ranges(), 0x30100, 16) && l.evicted() == 2);
}

// 32. A pending candidate the list already holds at its boundary is dropped untouched (no erase and reinsert, no
// generation move, no eviction). 0x10000+0x40 merges indices 0 and 1 only (index 2 at 0x10040 touches, and touching
// ranges do not merge: group 8); {0x2FFF0, 0x30} is a new range covering X.
static void TestHeldAtAdmission() {
  Case("held_at_admission");
  ss::Learner l;
  FullList(l, 1);
  l.BeginFrame(20);
  LearnL(l, kX.address, kX.size);  // refused (count)
  LearnL(l, 0x10000, 0x40);        // merges two: kMaxRanges - 1 ranges
  CHECK(HasRange(l.ranges(), 0x10000, 0x40) && HasRange(l.ranges(), 0x10040, 16) && l.ranges().size() == ss::kMaxRanges - 1);
  LearnL(l, 0x2FFF0, 0x30);
  CHECK(HasRange(l.ranges(), 0x2FFF0, 0x30) && l.ranges().size() == ss::kMaxRanges);
  uint64_t g = l.generation();
  size_t n = l.ranges().size();
  l.BeginFrame(21);  // no aging (idle 20 < 120); X is held: dropped
  CHECK(l.generation() == g);
  CHECK(l.evicted() == 0);
  CHECK(HasRange(l.ranges(), 0x2FFF0, 0x30));
  CHECK(l.ranges().size() == n);
}

// 33. Reset empties the pending list (a stale candidate is not admitted into the emptied list); evicted and
// refused_first survive it like the other refusal statistics.
static void TestResetClearsPending() {
  Case("reset_clears_pending");
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
  CHECK(StatField(*s, "ranges_now") == ss::kMaxRanges);
  Boundary(*s, 10, true, &inv);  // frame 11
  LearnS(*s, kX.address, kX.size);  // refused
  Boundary(*s, 11, true, &inv);  // frame 12: every range idle 10, X admitted
  CHECK(StatField(*s, "evicted") == 1);
  CHECK(StatField(*s, "ranges_now") == ss::kMaxRanges);
  LearnS(*s, 0x30100, 16);  // refused, pending
  s->Reset();
  Boundary(*s, 12, true, &inv);  // frame 13
  CHECK(StatField(*s, "ranges_now") == 0);  // NOT 1
  CHECK(StatField(*s, "evicted") == 1);
  CHECK(StatText(*s, "refused_first") == "0x30000:16:count");
}

// 34. refused_first is the FIRST cap refusal of the window; ClearWindowRefusal starts a new window.
static void TestRefusedFirstWindow() {
  Case("refused_first_window");
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
  CHECK(StatText(*s, "refused_first") == "-");
  LearnS(*s, kX.address, kX.size);
  LearnS(*s, 0x30100, 16);
  CHECK(StatText(*s, "refused_first") == "0x30000:16:count");
  s->ClearWindowRefusal();
  CHECK(StatText(*s, "refused_first") == "-");
  LearnS(*s, 0x30100, 16);
  CHECK(StatText(*s, "refused_first") == "0x30100:16:count");
}

// 35. The kind: the count cap is checked first. kMaxRanges ranges of kMaxTotalBytes / kMaxRanges bytes (512 x 1024 =
// 524288 at 512: both caps reached); one more range of 16 is over both (count kMaxRanges + 1, bytes 524304) and
// reads "count".
static void TestRefusedKindCountFirst() {
  Case("refused_kind_count_first");
  constexpr uint32_t kEach = ss::kMaxTotalBytes / ss::kMaxRanges;
  CHECK(kEach * ss::kMaxRanges == ss::kMaxTotalBytes && kEach < 0x1000);  // precondition: both caps exactly, gaps
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnS(*s, 0x100000 + i * 0x1000, kEach);
  CHECK(StatField(*s, "ranges_now") == ss::kMaxRanges && StatField(*s, "ranges_bytes") == 524288);
  LearnS(*s, 0x300000, 16);
  CHECK(StatText(*s, "refused_first") == "0x300000:16:count");
}

// 36. No allocation on the refusal and eviction path (group 18's rule): 20 frames, each refusing one candidate and
// admitting it at the next boundary by evicting an idle range.
static void TestNoAllocationOnEviction() {
  Case("no_allocation_on_eviction");
  auto s = std::make_unique<ss::State>();
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  for (uint32_t i = 0; i < ss::kMaxRanges; ++i) LearnS(*s, 0x10000 + i * 0x20, 16);
  g_allocs = 0;
  g_count_allocs = true;
  for (uint32_t k = 0; k < 20; ++k) {
    LearnS(*s, 0x50000 + k * 0x20, 16);  // refused (count)
    Boundary(*s, 20 + k, true, &inv);    // frame 21 + k: admitted by evicting an idle fill range
  }
  g_count_allocs = false;
  CHECK(g_allocs.load() == 0);
  CHECK(StatField(*s, "evicted") == 20);
}

// 8. Overlapping ranges merge; touching ones do not.
static void TestMergeOverlappingOnly() {
  Case("merge_overlapping_only");
  ss::Learner l;
  l.BeginFrame(1);
  LearnL(l, 0x100, 0x100);
  LearnL(l, 0x180, 0x100);
  CHECK(l.ranges().size() == 1 && HasRange(l.ranges(), 0x100, 0x180));
  uint64_t g = l.generation();
  LearnL(l, 0x140, 0x40);  // inside the merged range: the list is unchanged
  CHECK(l.generation() == g && l.ranges().size() == 1);
  LearnL(l, 0x400, 0x100);
  LearnL(l, 0x500, 0x100);
  CHECK(l.ranges().size() == 3);
  CHECK(HasRange(l.ranges(), 0x400, 0x100) && HasRange(l.ranges(), 0x500, 0x100));
  LearnL(l, 0x900, 0x10);  // learned out of address order: the list still comes back sorted
  LearnL(l, 0x700, 0x10);
  CHECK(l.ranges().size() == 5);
  bool sorted = true;
  for (size_t i = 1; i < l.ranges().size(); ++i) {
    if (l.ranges()[i - 1].address >= l.ranges()[i].address) sorted = false;
  }
  CHECK(sorted);
}

// 9. Aging: a range leaves at BeginFrame(f) when f - last_active > 120.
static void TestAgingEdge() {
  Case("aging_edge");
  {
    ss::Learner l;
    l.BeginFrame(10);
    LearnL(l, 0x1000, 64);
    uint64_t g = l.generation();
    l.BeginFrame(130);
    CHECK(HasRange(l.ranges(), 0x1000, 64));
    CHECK(l.generation() == g);  // an aging pass that removes nothing
    l.BeginFrame(131);
    CHECK(l.ranges().empty());
    CHECK(l.generation() == g + 1);
  }
  {
    ss::Learner l;
    l.BeginFrame(10);
    LearnL(l, 0x1000, 64);
    l.BeginFrame(100);
    l.NoteChanged({0x1010, 8});  // overlaps the range
    l.BeginFrame(220);
    CHECK(HasRange(l.ranges(), 0x1000, 64));
    l.BeginFrame(221);
    CHECK(l.ranges().empty());
  }
}

// 10. A range on a GPU-written page is dropped and banned; the ban ages out once the page stops being written.
static void TestBanGpuWritten() {
  Case("ban_gpu_written");
  WrittenFake wf{0x2000, 4096, true};
  {
    ss::Learner l;
    l.BeginFrame(1);
    LearnL(l, 0x2100, 0x100);  // inside the written page
    LearnL(l, 0x3000, 0x100);  // beside it
    uint64_t g = l.generation();
    l.BanOverlapping(WrittenFn, &wf, 12);
    CHECK(!HasRange(l.ranges(), 0x2100, 0x100));
    CHECK(HasRange(l.ranges(), 0x3000, 0x100));
    CHECK(l.generation() == g + 1);
    g = l.generation();
    l.BanOverlapping(WrittenFn, &wf, 12);  // removes nothing
    CHECK(l.generation() == g);
    l.BeginFrame(2);
    LearnL(l, 0x2180, 0x100);  // overlaps the banned range
    CHECK(!HasRange(l.ranges(), 0x2180, 0x100));
    LearnL(l, 0x2800, 0x100);  // the same GPU-written page, not overlapping the old range: the page is banned
    CHECK(!HasRange(l.ranges(), 0x2800, 0x100));
    LearnL(l, 0x4000, 0x100);  // beside it: learned
    CHECK(HasRange(l.ranges(), 0x4000, 0x100));
    // The page stops being GPU-written: the ban (last written at frame 1) leaves at frame 1 + 121.
    wf.on = false;
    for (uint32_t f = 3; f <= 121; ++f) {
      l.BeginFrame(f);
      l.BanOverlapping(WrittenFn, &wf, 12);
    }
    LearnL(l, 0x2100, 0x100);  // frame 121: still banned
    CHECK(!HasRange(l.ranges(), 0x2100, 0x100));
    l.BeginFrame(122);
    l.BanOverlapping(WrittenFn, &wf, 12);
    LearnL(l, 0x2100, 0x100);
    CHECK(HasRange(l.ranges(), 0x2100, 0x100));
  }
  {
    // Still written: the ban's last frame moves with it (written through frame 50: banned through 170).
    WrittenFake w2{0x2000, 4096, true};
    ss::Learner l;
    l.BeginFrame(1);
    LearnL(l, 0x2100, 0x100);
    for (uint32_t f = 1; f <= 170; ++f) {
      if (f > 1) l.BeginFrame(f);
      w2.on = f <= 50;
      l.BanOverlapping(WrittenFn, &w2, 12);
    }
    LearnL(l, 0x2100, 0x100);
    CHECK(!HasRange(l.ranges(), 0x2100, 0x100));
    l.BeginFrame(171);
    l.BanOverlapping(WrittenFn, &w2, 12);
    LearnL(l, 0x2100, 0x100);
    CHECK(HasRange(l.ranges(), 0x2100, 0x100));
  }
  {
    // The refusals are counted (A16): one removal, then one refused candidate.
    auto s = std::make_unique<ss::State>();
    InvLog inv;
    WrittenFake w3{0x2000, 4096, false};
    s->GpuFrameBoundary(1, true, InvFn, &inv, WrittenFn, &w3, 12);
    LearnS(*s, 0x2100, 0x100);
    CHECK(StatField(*s, "ranges_now") == 1);
    w3.on = true;
    s->GpuFrameBoundary(2, true, InvFn, &inv, WrittenFn, &w3, 12);
    CHECK(StatField(*s, "ranges_now") == 0);
    CHECK(StatField(*s, "banned") == 1);
    LearnS(*s, 0x2100, 0x100);
    CHECK(StatField(*s, "banned") == 2);
    CHECK(StatField(*s, "ranges_now") == 0);
  }
}

// 11. A list change takes effect from the next GuestSwap.
static void TestListChangeNextSwap() {
  Case("list_change_takes_effect_next_swap");
  {
    ss::Learner l;
    l.BeginFrame(1);
    uint64_t g = l.generation();
    LearnL(l, 0x1100, 64);
    CHECK(l.generation() == g + 1);
  }
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  LearnS(*s, 0x1100, 64);
  ss::CopyResult c8 = s->GuestSwap(8, MemRead, &mem, false);  // slot 0
  CHECK(c8.copied == 1);
  LearnS(*s, 0x1800, 64);
  ss::CopyResult c9 = s->GuestSwap(9, MemRead, &mem, false);  // slot 1
  CHECK(c9.copied == 2);
  Boundary(*s, 7, true, &inv);  // frame 8
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
  CHECK(AllPat(w, 0x1100, 64, true));
  CHECK(AllPat(w, 0x1800, 64, false));  // not in copy 8: live
  CHECK(StatField(*s, "not_in_copy") == 1);
  Boundary(*s, 8, true, &inv);  // frame 9
  Window w9(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w9.dest(), w9.Live(), 12);
  CHECK(AllPat(w9, 0x1100, 64, true));
  CHECK(AllPat(w9, 0x1800, 64, true));
  CHECK(StatField(*s, "not_in_copy") == 1);
}

// 13. An unreadable range is skipped: not held by the copy, its bytes stay live.
static void TestUnreadableSkipped() {
  Case("unreadable_range_skipped");
  ss::Ring ring;
  FakeMem mem(0x10000);
  mem.unreadable = 0x1200;
  const ss::Range list[2] = {{0x1100, 64}, {0x1200, 64}};
  ss::CopyResult c = ring.Copy(5, list, MemRead, &mem, false);
  CHECK(c.skipped_unreadable == 1);
  CHECK(c.copied == 1 && c.bytes == 64);
  std::vector<ss::Range> diff;
  Window w(0x1000, 4096);
  ss::SpliceResult r = ring.Splice(5, 0x1000, 4096, w.dest(), w.Live(), list, &diff);
  CHECK(r.missing == ss::Missing::kNone);
  CHECK(r.not_in_copy == 1);
  CHECK(AllPat(w, 0x1200, 64, false));
  CHECK(AllPat(w, 0x1100, 64, true));
}

// 14. The check re-reads each range after the copy and counts the ones that moved; off, one read per range.
static void TestCheckCountsMoved() {
  Case("check_counts_moved");
  ss::Ring ring;
  const ss::Range list[2] = {{0x1100, 64}, {0x1200, 64}};
  {
    FakeMem mem(0x10000);
    mem.flip_address = 0x1100;
    mem.flip_on_read = 2;  // the check's read sees a moved byte
    ss::CopyResult c = ring.Copy(1, list, MemRead, &mem, true);
    CHECK(c.copied == 2);
    CHECK(c.check_moved == 1);
    CHECK(mem.reads == 4);
  }
  {
    FakeMem mem(0x10000);
    mem.flip_address = 0x1100;
    mem.flip_on_read = 2;
    ss::CopyResult c = ring.Copy(2, list, MemRead, &mem, false);
    CHECK(c.copied == 2);
    CHECK(c.check_moved == 0);
    CHECK(mem.reads == 2);
  }
}

// 15. No current frame before the first ordinal: GpuFrameBoundary(0) splices nothing and counts nothing.
static void TestNoFrameBeforeFirstOrdinal() {
  Case("no_frame_before_first_ordinal");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  Boundary(*s, 5, true, &inv);  // frame 6: a frame to learn in
  LearnS(*s, 0x1100, 64);
  s->GuestSwap(1, MemRead, &mem, false);  // copy 1 (slot 1)
  s->GuestSwap(2, MemRead, &mem, false);  // copy 2 (slot 2)
  Boundary(*s, 0, true, &inv);            // no current frame (frame 1 would splice copy 1)
  std::string before = s->StatsLine();
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
  CHECK(AllPat(w, 0x1100, 64, false));
  CHECK(s->StatsLine() == before);
  // Positive companion (A13): the next boundary with an ordinal splices.
  Boundary(*s, 1, true, &inv);  // frame 2
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
  CHECK(AllPat(w, 0x1100, 64, true));
  CHECK(AllPat(w, 0x1000, 0x100, false));
}

// 16. Latched off: the kept span is still invalidated, nothing is learned or spliced; on again, both resume.
static void TestLatchedOff() {
  Case("latched_off_does_nothing_but_clears_kept");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  Boundary(*s, 1, true, &inv);  // frame 2
  CHECK(s->gpu_on());
  LearnS(*s, 0x1100, 64);
  s->GuestSwap(2, MemRead, &mem, false);
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);  // differs: a kept span on page 1
  CHECK(AllPat(w, 0x1100, 64, true));
  s->GuestSwap(3, MemRead, &mem, false);  // a copy for the frame after the off boundary (A6)
  inv = InvLog{};
  Boundary(*s, 2, false, &inv);
  CHECK(!s->gpu_on());
  CHECK(inv.n == 1 && inv.start[0] == 0x1000 && inv.length[0] == 0x1000);
  uint64_t spliced = StatField(*s, "spliced");
  uint64_t frames = StatField(*s, "frames");
  LearnS(*s, 0x1800, 64);
  CHECK(StatField(*s, "ranges_now") == 1);  // the list is kept, nothing learned
  Window w2(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w2.dest(), w2.Live(), 12);
  CHECK(AllPat(w2, 0x1100, 64, false));
  CHECK(StatField(*s, "spliced") == spliced);
  CHECK(StatField(*s, "frames") == frames);
  // On again (A13): learning and splicing resume.
  s->GuestSwap(4, MemRead, &mem, false);
  inv = InvLog{};
  Boundary(*s, 3, true, &inv);  // frame 4
  CHECK(inv.n == 0);
  LearnS(*s, 0x1800, 64);
  CHECK(StatField(*s, "ranges_now") == 2);
  Window w3(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w3.dest(), w3.Live(), 12);
  CHECK(AllPat(w3, 0x1100, 64, true));
}

// 17. The stats line, field by field, after a scripted sequence whose counts are derived in the comments.
static void TestStatsLine() {
  Case("stats_line_fields");
  {
    ss::State empty;
    CHECK(empty.StatsLine() ==
          "submit snapshot stats: frames=0 ranges_max=0 ranges_now=0 spliced=0 differing=0 missing_reused=0 "
          "missing_not_yet=0 not_in_copy=0 copy_us_median=0 copy_us_max=0 check_ranges=0 check_moved=0 "
          "swap_threads=other:0 rejected_cap=0 rejected_size=0 banned=0 kept_gpu_written=0 evicted=0 "
          "ranges_bytes=0 refused_first=-");
  }
  {
    // The overflow bucket reads as 2001: samples 5, 2500, 3000 -> sorted 5, ovf, ovf; the 2nd smallest is ovf.
    ss::State s;
    s.NoteCopyMicros(2500);
    s.NoteCopyMicros(3000);
    s.NoteCopyMicros(5);
    s.NoteSwapThread(42);
    CHECK(s.StatsLine() ==
          "submit snapshot stats: frames=0 ranges_max=0 ranges_now=0 spliced=0 differing=0 missing_reused=0 "
          "missing_not_yet=0 not_in_copy=0 copy_us_median=2001 copy_us_max=3000 check_ranges=0 check_moved=0 "
          "swap_threads=42:1,other:0 rejected_cap=0 rejected_size=0 banned=0 kept_gpu_written=0 evicted=0 "
          "ranges_bytes=0 refused_first=-");
  }
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x200000);
  InvLog inv;
  WrittenFake wf{0x9000, 4096, false};
  // swap_threads: 111 x3, 222 x2, 333 x1 -> "111:3,222:2,other:1".
  s->NoteSwapThread(111);
  s->NoteSwapThread(222);
  s->NoteSwapThread(111);
  s->NoteSwapThread(333);
  s->NoteSwapThread(222);
  s->NoteSwapThread(111);
  // Copy times 5, 9, 3000, 7: sorted 5, 7, 9, ovf; n = 4, the 2nd smallest (lower median) = 7; max 3000.
  s->NoteCopyMicros(5);
  s->NoteCopyMicros(9);
  s->NoteCopyMicros(3000);
  s->NoteCopyMicros(7);
  s->GpuFrameBoundary(1, true, InvFn, &inv, WrittenFn, &wf, 12);  // frames=1, frame 2
  LearnS(*s, 0x1100, 384);                                    // R1
  LearnS(*s, 0x1800, 768);                                    // R2
  LearnS(*s, 0x9000, 16);                                     // R3 (to be banned)
  for (uint32_t i = 0; i < 7; ++i) LearnS(*s, 0x100000 + i * 0x18000, 65536);  // 10 ranges, 459920 bytes
  LearnS(*s, 0x100000 + 7 * 0x18000, 65536);  // 459920 + 65536 = 525456 > 524288: rejected_cap=1
  LearnS(*s, 0x80000, 65537);                 // rejected_size=1
  // ranges_max=10. GuestSwap(7) with the check: 10 ranges checked; R1's byte moves before its 2nd read.
  mem.flip_address = 0x1100;
  mem.flip_on_read = 2;
  ss::CopyResult c7 = s->GuestSwap(7, MemRead, &mem, true);  // check_ranges=10, check_moved=1
  CHECK(c7.copied == 10 && c7.check_moved == 1);
  wf.on = true;
  s->GpuFrameBoundary(2, true, InvFn, &inv, WrittenFn, &wf, 12);  // frames=2, frame 3; R3 dropped: banned=1, 9 ranges
  wf.on = false;
  s->GuestSwap(8, MemRead, &mem, false);                      // slot 0; no check
  s->GpuFrameBoundary(6, true, InvFn, &inv, WrittenFn, &wf, 12);  // frames=3, frame 7 (slot 3 = copy 7)
  Window wa(0x1000, 4096);                                    // pattern B: R1 and R2 differ from copy 7
  s->GpuSplice(0x1000, 4096, wa.dest(), wa.Live(), 12);                  // spliced=1, differing=1, not_in_copy +0
  s->GpuFrameBoundary(8, true, InvFn, &inv, WrittenFn, &wf, 12);  // frames=4, frame 9 (slot 1: never written)
  Window wb(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, wb.dest(), wb.Live(), 12);  // missing_not_yet=1
  s->GuestSwap(13, MemRead, &mem, false);     // slot 1 = 13 > 9
  s->GpuSplice(0x1000, 4096, wb.dest(), wb.Live(), 12);  // missing_reused=1
  s->GuestSwap(10, MemRead, &mem, false);     // slot 2 = copy 10 (9 ranges, memory after the flip)
  s->GpuFrameBoundary(9, true, InvFn, &inv, WrittenFn, &wf, 12);  // frames=5, frame 10
  LearnS(*s, 0x1400, 32);                                     // R4: 10 ranges again (ranges_max stays 10)
  Window wc(0x1000, 4096);
  wc.FillFrom(mem);                           // copy == live for R1 and R2
  s->GpuSplice(0x1000, 4096, wc.dest(), wc.Live(), 12);  // spliced=2, differing stays 1, R4 not in copy 10: not_in_copy=1
  s->NoteKeptGpuWritten(2);  // kept_gpu_written=2 (review N1; a missing call would read 0)
  // evicted=0: the frame-2 refusal (0x1A8000, 65536, refused by the bytes cap) was pending at boundary 3, where every
  // range was idle 1, so it was dropped. ranges_bytes = R1 384 + R2 768 + 7 x 65536 + R4 32 = 459936 (R3 banned).
  // refused_first: the window's first cap refusal (the rejected_size one is not a cap refusal).
  CHECK(s->StatsLine() ==
        "submit snapshot stats: frames=5 ranges_max=10 ranges_now=10 spliced=2 differing=1 missing_reused=1 "
        "missing_not_yet=1 not_in_copy=1 copy_us_median=7 copy_us_max=3000 check_ranges=10 check_moved=1 "
        "swap_threads=111:3,222:2,other:1 rejected_cap=1 rejected_size=1 banned=1 kept_gpu_written=2 evicted=0 "
        "ranges_bytes=459936 refused_first=0x1A8000:65536:bytes");
}

// 18. No allocation per frame after one warm-up frame (A9).
static void TestNoAllocationAfterWarmup() {
  Case("no_allocation_after_warmup");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  // The instrument counts (positive control). An explicit operator new call: a new-expression whose result is
  // deleted unused may be elided by the compiler (clang does at /O2).
  g_allocs = 0;
  g_count_allocs = true;
  void* volatile probe = ::operator new(16);
  g_count_allocs = false;
  CHECK(g_allocs.load() == 1);
  ::operator delete(probe);
  // Warm-up frame.
  Boundary(*s, 1, true, &inv);  // frame 2
  LearnS(*s, 0x1100, 64);
  LearnS(*s, 0x1800, 64);
  s->GuestSwap(2, MemRead, &mem, false);
  s->GuestSwap(3, MemRead, &mem, false);
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
  Window w2(0x1000, 4096);
  // Scripted frame.
  g_allocs = 0;
  g_count_allocs = true;
  Boundary(*s, 2, true, &inv);  // frame 3
  LearnS(*s, 0x1100, 64);       // no change to the list
  s->GuestSwap(4, MemRead, &mem, false);
  s->GpuSplice(0x1000, 4096, w2.dest(), w2.Live(), 12);
  g_count_allocs = false;
  CHECK(g_allocs.load() == 0);
  CHECK(AllPat(w2, 0x1100, 64, true) && AllPat(w2, 0x1800, 64, true));  // the frame did its work
}

// 20. Reset clears the learner, the bans, the published list, the slots' ordinals and the kept spans (A18).
static void TestResetClears() {
  Case("reset_clears");
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  WrittenFake wf{0x9000, 4096, false};
  s->GpuFrameBoundary(1, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 2
  LearnS(*s, 0x9000, 16);
  wf.on = true;
  s->GpuFrameBoundary(2, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 3: banned
  wf.on = false;
  CHECK(StatField(*s, "banned") == 1);
  LearnS(*s, 0x1100, 64);
  s->GuestSwap(3, MemRead, &mem, false);  // slot 3
  s->GuestSwap(4, MemRead, &mem, false);  // slot 0
  Window w(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);  // differs: a kept span
  CHECK(AllPat(w, 0x1100, 64, true));
  CHECK(StatField(*s, "ranges_now") == 1);
  s->NoteKeptGpuWritten(3);
  s->Reset();
  CHECK(StatField(*s, "ranges_now") == 0);  // learner cleared
  CHECK(StatField(*s, "kept_gpu_written") == 3);  // a statistic of the run: survives (review N1)
  inv = InvLog{};
  s->GpuFrameBoundary(3, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 4
  CHECK(inv.n == 0);                                          // kept spans cleared
  mem.reads = 0;
  ss::CopyResult c5 = s->GuestSwap(5, MemRead, &mem, false);  // slot 1
  CHECK(c5.copied == 0 && mem.reads == 0);                    // published list cleared
  LearnS(*s, 0x1100, 64);
  Window w4(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, w4.dest(), w4.Live(), 12);  // copy 4 existed before Reset: now "never written"
  CHECK(AllPat(w4, 0x1100, 64, false));
  CHECK(StatField(*s, "missing_not_yet") == 1);
  LearnS(*s, 0x9000, 16);  // the ban is gone
  CHECK(StatField(*s, "ranges_now") == 2);
  CHECK(StatField(*s, "banned") == 1);
  // The learner stays on the current frame (review P2): a range learned after a mid-frame Reset at frame 200 is
  // still listed at frame 201. The same sequence without Reset is the control.
  for (int with_reset = 0; with_reset < 2; ++with_reset) {
    auto r = std::make_unique<ss::State>();
    InvLog li;
    Boundary(*r, 199, true, &li);  // frame 200
    if (with_reset) r->Reset();
    LearnS(*r, 0x1100, 64);
    CHECK(StatField(*r, "ranges_now") == 1);
    Boundary(*r, 200, true, &li);  // frame 201
    CHECK(StatField(*r, "ranges_now") == 1);
  }
}

// 21. Only bytes a held range shares with a range learned NOW are spliced (review F1, F3).
static void TestSpliceOnlyLearnedNow() {
  Case("splice_only_learned_now");
  {
    // Review P1: a range banned after the copy stays live, also beside a learned range in the same window.
    auto s = std::make_unique<ss::State>();
    FakeMem mem(0x10000);
    InvLog inv;
    WrittenFake wf{0x1800, 64, false};
    s->GpuFrameBoundary(1, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 2
    LearnS(*s, 0x1100, 64);
    LearnS(*s, 0x1800, 64);
    s->GuestSwap(3, MemRead, &mem, false);  // copy 3 holds both
    wf.on = true;
    s->GpuFrameBoundary(2, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 3: 0x1800 dropped and banned
    CHECK(StatField(*s, "ranges_now") == 1);
    CHECK(StatField(*s, "banned") == 1);
    Window w(0x1000, 4096);
    s->GpuSplice(0x1000, 4096, w.dest(), w.Live(), 12);
    CHECK(AllPat(w, 0x1100, 64, true));   // control: the learned range is spliced
    CHECK(AllPat(w, 0x1800, 64, false));  // the dropped range stays live
    Window w2(0x1800, 64);
    s->GpuSplice(0x1800, 64, w2.dest(), w2.Live(), 12);
    CHECK(AllPat(w2, 0x1800, 64, false));  // alone in its window: stays live
  }
  auto s = std::make_unique<ss::State>();
  FakeMem mem(0x10000);
  InvLog inv;
  WrittenFake wf{0x1100, 64, false};
  s->GpuFrameBoundary(1, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 2
  LearnS(*s, 0x1100, 64);                                         // R1 (to be banned)
  LearnS(*s, 0x5100, 64);                                         // R2
  s->GuestSwap(3, MemRead, &mem, false);                          // copy 3 holds R1 and R2
  wf.on = true;
  s->GpuFrameBoundary(2, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 3: R1 dropped
  wf.on = false;
  CHECK(StatField(*s, "ranges_now") == 1);
  // (a) A window overlapping no learned range while copy 3 holds bytes there: nothing spliced, nothing counted.
  Window wa(0x1000, 4096);
  s->GpuSplice(0x1000, 4096, wa.dest(), wa.Live(), 12);
  CHECK(AllPat(wa, 0x1100, 64, false));
  CHECK(StatField(*s, "spliced") == 0);
  CHECK(StatField(*s, "not_in_copy") == 0);
  s->GuestSwap(5, MemRead, &mem, false);                          // copy 5 holds R2 only
  s->GpuFrameBoundary(3, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 4: no copy 4
  s->GpuSplice(0x1000, 4096, wa.dest(), wa.Live(), 12);
  CHECK(StatField(*s, "missing_not_yet") == 0);  // no learned range in the window: not a missing copy
  CHECK(StatField(*s, "missing_reused") == 0);
  Window wr(0x5000, 4096);
  s->GpuSplice(0x5000, 4096, wr.dest(), wr.Live(), 12);
  CHECK(StatField(*s, "missing_not_yet") == 1);  // control: R2's window counts the missing copy
  // (b) A learned range in the window that the copy does not hold there: not counted as spliced.
  LearnS(*s, 0x6800, 64);                                         // R3, after copy 5
  s->GpuFrameBoundary(4, true, InvFn, &inv, WrittenFn, &wf, 12);  // frame 5
  Window wb(0x6000, 4096);
  s->GpuSplice(0x6000, 4096, wb.dest(), wb.Live(), 12);
  CHECK(AllPat(wb, 0x6800, 64, false));
  CHECK(StatField(*s, "spliced") == 0);
  CHECK(StatField(*s, "not_in_copy") == 1);
  s->GpuSplice(0x5000, 4096, wr.dest(), wr.Live(), 12);  // control: R2 is held and learned
  CHECK(StatField(*s, "spliced") == 1);
  CHECK(AllPat(wr, 0x5100, 64, true));
}

// A third byte pattern, different from A and B at every address.
static uint8_t PatC(uint32_t a) { return uint8_t((a & 0xFF) ^ 0x5F); }

// 22. differing compares the copy with live guest memory, never with dest (review I1): dest (an upload mapping,
// write-combined or uncached in the SDK) is only written. dest is pre-filled with pattern C, which differs from the
// copy (A) and from live; live equals the copy for one piece and differs (B) for the other. The likeliest wrong
// version, comparing with dest (the code before fix round 1), lists both pieces; a version that skips the write
// where copy == live leaves pattern C in the first piece.
static void TestDifferingAgainstLive() {
  Case("differing_against_live_not_dest");
  FakeMem mem(0x10000);  // pattern A: the copy's bytes
  {
    ss::Ring ring;
    const ss::Range list[2] = {{0x1100, 64}, {0x1800, 64}};
    ss::CopyResult c = ring.Copy(1, list, MemRead, &mem, false);
    CHECK(c.copied == 2);
    Window w(0x1000, 4096);
    std::vector<uint8_t> live(4096);
    for (uint32_t i = 0; i < 4096; ++i) {
      w.dest()[i] = PatC(0x1000 + i);
      live[i] = PatC(0x1000 + i);
    }
    for (uint32_t i = 0; i < 64; ++i) {
      live[0x100 + i] = PatA(0x1100 + i);  // piece 1: live == copy
      live[0x800 + i] = PatB(0x1800 + i);  // piece 2: live != copy
    }
    bool dest_differs = true;  // precondition: dest differs from the copy and from live in both pieces
    for (uint32_t a : {0x1100u, 0x1800u}) {
      for (uint32_t i = 0; i < 64; ++i) {
        if (w.at(a + i) == PatA(a + i) || w.at(a + i) == live[a - 0x1000 + i]) dest_differs = false;
      }
    }
    CHECK(dest_differs);
    std::vector<ss::Range> diff;
    ss::SpliceResult r = ring.Splice(1, 0x1000, 4096, w.dest(), live.data(), list, &diff);
    CHECK(r.missing == ss::Missing::kNone && r.spliced == 2);
    CHECK(r.differing == 1);  // NOT 2 (compared with dest), NOT 0
    CHECK(diff.size() == 1 && diff[0].address == 0x1800 && diff[0].size == 64);
    CHECK(AllPat(w, 0x1100, 64, true) && AllPat(w, 0x1800, 64, true));  // both pieces written with the copy
    bool rest_c = true;  // the rest of dest is untouched (not refilled from live)
    for (uint32_t i = 0; i < 4096; ++i) {
      const uint32_t a = 0x1000 + i;
      if ((a >= 0x1100 && a < 0x1140) || (a >= 0x1800 && a < 0x1840)) continue;
      if (w.at(a) != PatC(a)) rest_c = false;
    }
    CHECK(rest_c);
    CHECK(w.CanariesIntact());
  }
  {
    // Through State::GpuSplice: only the differing piece's page is kept (page 2), not page 1 as well.
    auto s = std::make_unique<ss::State>();
    InvLog inv;
    Boundary(*s, 1, true, &inv);  // frame 2
    LearnS(*s, 0x1100, 64);       // page 1
    LearnS(*s, 0x2800, 64);       // page 2
    s->GuestSwap(2, MemRead, &mem, false);
    Window w(0x1000, 0x2000);
    std::vector<uint8_t> live(0x2000);
    for (uint32_t i = 0; i < 0x2000; ++i) {
      w.dest()[i] = PatC(0x1000 + i);
      live[i] = PatC(0x1000 + i);
    }
    for (uint32_t i = 0; i < 64; ++i) {
      live[0x100 + i] = PatA(0x1100 + i);   // live == copy
      live[0x1800 + i] = PatB(0x2800 + i);  // live != copy
    }
    s->GpuSplice(0x1000, 0x2000, w.dest(), live.data(), 12);
    CHECK(StatField(*s, "spliced") == 1);
    CHECK(AllPat(w, 0x1100, 64, true) && AllPat(w, 0x2800, 64, true));
    inv = InvLog{};
    Boundary(*s, 2, true, &inv);  // frame 3: the kept spans are invalidated
    CHECK(inv.n == 1 && inv.start[0] == 0x2000 && inv.length[0] == 0x1000);  // NOT {0x1000, 0x2000}
  }
}

struct RunLog {
  int n = 0;
  uint32_t first[8] = {};
  uint32_t count[8] = {};
};

static void RunFn(void* ctx, uint32_t first, uint32_t count) {
  auto* l = static_cast<RunLog*>(ctx);
  if (l->n < 8) {
    l->first[l->n] = first;
    l->count[l->n] = count;
  }
  ++l->n;
}

// 23. ForEachClearPageRun (review M2's walk): runs of pages whose bit is clear, across a 64-page word edge, clamped
// to the words held. Bits set: pages 62, 64, 65 (bit 62 of word 0, bits 0 and 1 of word 1).
static void TestClearPageRuns() {
  Case("clear_page_runs");
  const uint64_t bits[2] = {uint64_t(1) << 62, 0x3};
  // The return value (review N1) is the number of set (skipped) pages in the clamped span. The likeliest wrong
  // version, page_count minus the pages handed to run(), differs from it on every case below but the all-set one.
  {
    RunLog l;
    const uint32_t skipped = ss::ForEachClearPageRun(bits, 60, 11, RunFn, &l);  // pages 60..70
    // NOT one run {60, 11} (bits ignored), NOT {62,1},{64,2} (set runs), the last run emitted.
    CHECK(l.n == 3);
    CHECK(l.n == 3 && l.first[0] == 60 && l.count[0] == 2);
    CHECK(l.n == 3 && l.first[1] == 63 && l.count[1] == 1);
    CHECK(l.n == 3 && l.first[2] == 66 && l.count[2] == 5);
    CHECK(skipped == 3);  // pages 62, 64, 65
  }
  {
    RunLog l;
    const uint32_t skipped = ss::ForEachClearPageRun(bits, 64, 2, RunFn, &l);  // every page set: no run
    CHECK(l.n == 0);
    CHECK(skipped == 2);
  }
  {
    RunLog l;
    const uint32_t skipped = ss::ForEachClearPageRun(bits, 120, 20, RunFn, &l);  // 128 pages held: 120..127
    CHECK(l.n == 1 && l.first[0] == 120 && l.count[0] == 8);
    CHECK(skipped == 0);  // NOT 12 (20 - 8)
    const uint64_t tail[2] = {0, uint64_t(1) << 63};  // page 127 set
    RunLog t;
    const uint32_t tail_skipped = ss::ForEachClearPageRun(tail, 120, 20, RunFn, &t);
    CHECK(t.n == 1 && t.first[0] == 120 && t.count[0] == 7);
    CHECK(tail_skipped == 1);  // NOT 13 (20 - 7)
  }
  {
    RunLog l;
    const uint32_t past = ss::ForEachClearPageRun(bits, 200, 5, RunFn, &l);  // past the words: nothing
    const uint32_t none = ss::ForEachClearPageRun(bits, 0, 0, RunFn, &l);    // no pages: nothing
    CHECK(l.n == 0);
    CHECK(past == 0 && none == 0);  // NOT 5 for the span past the words
  }
}

// Waits until flag is set, polling, for at most ms milliseconds.
static bool WaitFlag(const std::atomic<bool>& flag, int ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// A stuck thread: report it and leave without joining (A2).
[[noreturn]] static void ExitStuck(const char* what) {
  std::printf("FAIL %s:%d: thread stuck past its deadline: %s\n", __FILE__, __LINE__, what);
  ++g_failures;
  Summary();
  std::_Exit(1);
}

struct BlockingRead {
  FakeMem* mem;
  std::atomic<bool> inside{false};
  std::atomic<bool> release{false};
  int calls = 0;
};

static bool BlockingReadFn(void* ctx, uint32_t address, uint32_t size, uint8_t* dest) {
  auto* b = static_cast<BlockingRead*>(ctx);
  if (b->calls++ == 0) {
    b->inside = true;  // inside the first range, slot held
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!b->release.load() && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return MemRead(b->mem, address, size, dest);
}

// 12. The GPU thread's splice waits for the slot while the guest copies into it (A14).
static void TestSlotLockedWhileCopying() {
  Case("slot_locked_while_copying");
  auto ring = std::make_unique<ss::Ring>();
  FakeMem mem(0x10000);
  const ss::Range r{0x1100, 64};
  ring->Copy(4, {&r, 1}, MemRead, &mem, false);  // slot 0 = 4
  // Copy 8 reads pattern B, so a splice that read it would also change dest.
  FakeMem memb(0x10000);
  for (uint32_t a = 0; a < 0x10000; ++a) memb.bytes[a] = PatB(a) ^ 0x01;
  BlockingRead br{&memb};
  std::atomic<bool> copy_done{false};
  std::thread guest([&] {
    ring->Copy(8, {&r, 1}, BlockingReadFn, &br, false);
    copy_done = true;
  });
  bool inside = WaitFlag(br.inside, 2000);
  CHECK(inside);
  if (!inside) {
    if (!WaitFlag(copy_done, 2000)) ExitStuck("guest copy");
    guest.join();
    return;
  }
  Window w(0x1000, 4096);
  std::vector<uint8_t> saved(w.dest(), w.dest() + 4096);
  std::atomic<bool> splice_done{false};
  ss::SpliceResult res{ss::Missing::kNone, 0, 0, 0};
  std::vector<ss::Range> diff;
  std::thread gpu([&] {
    res = ring->Splice(4, 0x1000, 4096, w.dest(), w.Live(), {&r, 1}, &diff);
    splice_done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(!splice_done.load());
  br.release = true;
  if (!WaitFlag(splice_done, 2000)) ExitStuck("gpu splice");
  if (!WaitFlag(copy_done, 2000)) ExitStuck("guest copy");
  gpu.join();
  guest.join();
  CHECK(res.missing == ss::Missing::kReused);
  CHECK(std::memcmp(saved.data(), w.dest(), 4096) == 0);
}

struct Recorder {
  ss::Range seen[600];
  int n = 0;
};

static bool RecordRead(void* ctx, uint32_t address, uint32_t size, uint8_t* dest) {
  auto* rec = static_cast<Recorder*>(ctx);
  if (rec->n < 600) rec->seen[rec->n] = {address, size};
  ++rec->n;
  std::memset(dest, 0x5A, size);
  return true;
}

// 19. The list GuestSwap copies is always a whole published list while the GPU thread changes it (A11).
static void TestPublishUnderConcurrency() {
  Case("publish_under_concurrency");
  auto s = std::make_unique<ss::State>();
  std::atomic<bool> stop{false};
  std::atomic<bool> gpu_done{false};
  std::thread gpu([&] {
    uint32_t f = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      s->GpuFrameBoundary(f, true, NoInv, nullptr, NeverWritten, nullptr, 12);
      // One new range a frame at a cycling address; each leaves 121 frames later: the list keeps changing.
      uint32_t slot = (f * 37) % 300;
      LearnS(*s, 0x10000 + slot * 0x100, 0x80);
      ++f;
    }
    gpu_done = true;
  });
  int swaps = 0, invalid = 0, nonempty = 0, size_changes = 0, last_size = -1;
  Recorder rec;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  uint32_t ordinal = 1;
  while (std::chrono::steady_clock::now() < end) {
    rec.n = 0;
    s->GuestSwap(ordinal, RecordRead, &rec, false);
    ordinal = ordinal == 0xFFFFFFFFu ? 1 : ordinal + 1;
    ++swaps;
    bool ok = rec.n <= int(ss::kMaxRanges);
    uint64_t bytes = 0;
    for (int i = 0; ok && i < rec.n; ++i) {
      bytes += rec.seen[i].size;
      if (i > 0 && uint64_t(rec.seen[i - 1].address) + rec.seen[i - 1].size > rec.seen[i].address) ok = false;
    }
    if (bytes > ss::kMaxTotalBytes) ok = false;
    if (!ok) ++invalid;
    if (rec.n > 0) ++nonempty;
    if (rec.n != last_size) ++size_changes;
    last_size = rec.n;
  }
  stop = true;
  if (!WaitFlag(gpu_done, 2000)) ExitStuck("gpu learner loop");
  gpu.join();
  std::printf("publish_under_concurrency: %d swaps, %d non-empty, %d size changes, %d invalid\n", swaps, nonempty,
              size_changes, invalid);
  CHECK(invalid == 0);
  CHECK(nonempty > 10);
  CHECK(size_changes > 2);
}

int main() {
  TestSpliceTwoRanges();
  TestRangeAcrossPageEdge();
  TestCopyMissingReasons();
  TestKeptSpans();
  TestLearnedPageSpans();
  TestLearningSignature();
  TestLimits();
  TestCountCapIs512();
  TestEvictIdleEdge();
  TestEvictLongestIdle();
  TestNothingIdleRefuses();
  TestPendingBoundAndDedup();
  TestEvictOverlapNotVictim();
  TestBytesCapEvictsSeveral();
  TestBanBeforeAdmission();
  TestPartialEvictThenDrop();
  TestFutileEvictionDrops();
  TestPreCheckAtByteEdge();
  TestPreCheckSkipsOverlap();
  TestPreCheckEveryCandidate();
  TestR2AgingBeforeAdmission();
  TestR3PendingOrder();
  TestR4PendingSurvivesFrameZero();
  TestIdleAtCounterWrap();
  TestHeldAtAdmission();
  TestResetClearsPending();
  TestRefusedFirstWindow();
  TestRefusedKindCountFirst();
  TestNoAllocationOnEviction();
  TestMergeOverlappingOnly();
  TestAgingEdge();
  TestBanGpuWritten();
  TestListChangeNextSwap();
  TestUnreadableSkipped();
  TestCheckCountsMoved();
  TestNoFrameBeforeFirstOrdinal();
  TestLatchedOff();
  TestStatsLine();
  TestNoAllocationAfterWarmup();
  TestResetClears();
  TestSpliceOnlyLearnedNow();
  TestDifferingAgainstLive();
  TestClearPageRuns();
  // The threaded groups last, so a stuck thread's early exit leaves every other group's result printed.
  TestSlotLockedWhileCopying();
  TestPublishUnderConcurrency();
  return Summary();
}
