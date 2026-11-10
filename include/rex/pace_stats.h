/**
 * Frame pacing statistics (diagnostic). Enabled by the environment variable
 * REX_PACE_STATS=1; costs one relaxed load per mark when off.
 *
 * Channels: guest vblank (MarkVblank), guest swap (IssueSwap, on the GPU
 * command processor thread), host present (after IDXGISwapChain::Present),
 * guest VdSwap (the guest thread's call, before the GPU thread reaches it). Every 600 marks of a channel, logs the
 * interval distribution in milliseconds: average, min, max, and how many
 * intervals were short (< 8 ms, a burst) or long (> 17.5 ms, > 25 ms).
 * The present channel also logs how many presents showed a new guest frame
 * and the latency from a guest swap to the first present after it.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>

namespace rex::pace_stats {

enum Channel : int { kVblank = 0, kSwap = 1, kPresent = 2, kGuestSwap = 3, kChannelCount = 4 };

// ---- Lag marks (the title's latency trace; exported as rex_lag_*, d3d12_presenter.cpp) ----
// Off unless the title calls rex_lag_marks_enable(1); off, each site below costs one relaxed load and one branch.
// On, every guest VdSwap gets an ordinal (from 1) that travels in its PM4_XE_SWAP packet (a fifth data dword) to
// the GPU thread, then in the guest output's properties (Presenter::GuestOutputProperties::lag_swap_index) to the
// host present that paints it. Times are QueryPerformanceCounter ticks.
// The GPU command processor thread's view of one swap packet (PM4_XE_SWAP), filled on that thread
// (command_processor.cpp) and carried with the guest output it refreshes (GuestOutputProperties::lag_gpu) to the
// present that paints it. A "wait" is a WAIT_REG_MEM whose condition did not hold at its first check.
struct LagGpuSwap {
  int64_t qpc_swap = 0;        // the swap packet's handler entry (IssueSwap follows at once)
  int64_t qpc_wait_enter = 0;  // the last wait since the previous swap packet: its first unmet check
  int64_t qpc_wait_wake = 0;   // ... and the check at which its condition held
  int64_t qpc_vblank = 0;      // the last guest vblank (GraphicsSystem::MarkVblank) before that wake
  int64_t wait_ticks = 0;      // every wait since the previous swap packet, summed (QPC ticks)
  uint32_t waits = 0;          // how many waits since the previous swap packet
  uint32_t wait_poll = 0;      // the last wait's poll address (memory: low 2 bits are the endianness)
  uint32_t wait_ref = 0;       // ... its reference value
  uint32_t wait_info = 0;      // ... its wait_info (bit 4: a memory poll; bits 0-2: the compare function)
  uint32_t after_wait_packets = 0;  // packets executed after that wake up to and including the swap packet (type 0, 1
                                    // and 3; type-2 and PM4_NOP packets not counted)
  uint32_t after_wait_sig = 0;      // the first four of them, one byte each from the low byte: a type-3 packet's
                                    // opcode, 0x80 for a type-0 (register write) packet, 0x81 for a type-1 packet
  uint32_t flags = 0;               // kLagGpuSwapEarly
  uint32_t reserved = 0;
};
static_assert(sizeof(LagGpuSwap) == 72);
// LagGpuSwap::flags bit 0: the swap was presented at the wait before its packet (present_before_vblank_wait); the
// record's qpc_swap is then that early IssueSwap and the wait's wake is not known yet (0).
constexpr uint32_t kLagGpuSwapEarly = 1;

struct LagPresentMark {
  int64_t qpc_before = 0;   // just before IDXGISwapChain::Present
  int64_t qpc_after = 0;    // just after it returned
  uint32_t swap_index = 0;  // the VdSwap ordinal of the guest output this present painted; 0: none painted
  uint32_t flags = 0;       // bit 0: the swap chain allows tearing (swap_chain_allows_tearing); bit 1: presented in
                            // DXGI exclusive fullscreen (no tearing flag passed, the UI thread presented); bit 2:
                            // presented with sync interval 1 (present_vsync; no tearing flag passed)
  int32_t present_hr = 0;   // what Present returned
  uint32_t reserved = 0;
  LagGpuSwap gpu;           // the GPU thread's marks of that guest output's swap packet (all 0: none painted)
};
static_assert(sizeof(LagPresentMark) == 104);

inline std::atomic<bool>& LagMarksOn() {
  static std::atomic<bool> on{false};
  return on;
}
inline bool LagOn() { return LagMarksOn().load(std::memory_order_relaxed); }
// The last VdSwap ordinal handed out (the guest threads that swap: the title's main thread, the D3D kick thread).
inline std::atomic<uint32_t>& LagGuestSwapCount() {
  static std::atomic<uint32_t> count{0};
  return count;
}
// The calling thread's last VdSwap while the marks were on: its ordinal and the QPC time it was handed out.
struct LagThreadSwap {
  uint32_t index = 0;
  int64_t qpc = 0;
};
inline LagThreadSwap& LagLastSwapThisThread() {
  thread_local LagThreadSwap last;
  return last;
}
// The ordinal of the swap packet the GPU thread is executing (written and read on the GPU thread, before and inside
// IssueSwap's RefreshGuestOutput).
inline uint32_t& LagGpuSwapIndex() {
  static uint32_t index = 0;
  return index;
}
// The GPU thread's marks (written and read on the GPU thread only): the swap being built since the previous swap
// packet, and the record of the last swap packet (read by IssueSwap's RefreshGuestOutput).
inline LagGpuSwap& LagGpuPending() {
  static LagGpuSwap pending;
  return pending;
}
inline LagGpuSwap& LagGpuLastSwap() {
  static LagGpuSwap last;
  return last;
}
// QPC time of the last guest vblank (the GPU VSync thread writes it while the marks are on).
inline std::atomic<int64_t>& LagLastVblankQpc() {
  static std::atomic<int64_t> qpc{0};
  return qpc;
}

inline bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REX_PACE_STATS");
    return v && v[0] && v[0] != '0';
  }();
  return enabled;
}

inline int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct ChannelState {
  int64_t last_us = 0;
  int64_t sum_us = 0;
  int64_t min_us = INT64_MAX;
  int64_t max_us = 0;
  int n = 0;
  int burst = 0;
  int over_17_5 = 0;
  int over_25 = 0;
  int latency_n = 0;
  int64_t latency_sum_us = 0;
  int64_t latency_max_us = 0;
};

inline std::atomic<int64_t>& LastSwapUs() {
  static std::atomic<int64_t> last_swap_us{0};
  return last_swap_us;
}

// Each channel is marked from one thread at a time (vblank: the VSync thread,
// swap: the GPU command processor thread, present: whichever thread presents),
// so the per-channel state needs no lock.
inline void Mark(Channel channel) {
  if (!Enabled()) return;
  static ChannelState states[kChannelCount];
  static const char* const names[kChannelCount] = {"vblank", "swap", "present", "guest VdSwap"};
  const int64_t now = NowUs();
  if (channel == kSwap) LastSwapUs().store(now, std::memory_order_relaxed);
  ChannelState& s = states[channel];
  if (channel == kPresent) {
    // Only the first present after a guest swap shows that frame for the
    // first time; later presents repeat it (UI repaints).
    const int64_t swap = LastSwapUs().exchange(0, std::memory_order_relaxed);
    if (swap) {
      const int64_t latency = now - swap;
      s.latency_n++;
      s.latency_sum_us += latency;
      s.latency_max_us = std::max(s.latency_max_us, latency);
    }
  }
  if (s.last_us) {
    const int64_t d = now - s.last_us;
    s.sum_us += d;
    s.min_us = std::min(s.min_us, d);
    s.max_us = std::max(s.max_us, d);
    if (d < 8000) s.burst++;
    if (d > 17500) s.over_17_5++;
    if (d > 25000) s.over_25++;
    if (++s.n == 600) {
      if (channel == kPresent) {
        REXLOG_INFO(
            "[pace] {}: avg {:.2f} min {:.2f} max {:.2f} ms; <8ms {} >17.5ms {} >25ms {} of 600;"
            " {} new frames, swap->first present avg {:.2f} max {:.2f} ms",
            names[channel], s.sum_us / 600.0 / 1000.0, s.min_us / 1000.0, s.max_us / 1000.0,
            s.burst, s.over_17_5, s.over_25, s.latency_n,
            s.latency_n ? s.latency_sum_us / double(s.latency_n) / 1000.0 : 0.0,
            s.latency_max_us / 1000.0);
      } else {
        REXLOG_INFO("[pace] {}: avg {:.2f} min {:.2f} max {:.2f} ms; <8ms {} >17.5ms {} >25ms {} of 600",
                    names[channel], s.sum_us / 600.0 / 1000.0, s.min_us / 1000.0,
                    s.max_us / 1000.0, s.burst, s.over_17_5, s.over_25);
      }
      const int64_t keep_last = now;
      s = ChannelState{};
      s.last_us = keep_last;
      return;
    }
  }
  s.last_us = now;
}

}  // namespace rex::pace_stats
