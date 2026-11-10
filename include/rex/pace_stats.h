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
struct LagPresentMark {
  int64_t qpc_before = 0;   // just before IDXGISwapChain::Present
  int64_t qpc_after = 0;    // just after it returned
  uint32_t swap_index = 0;  // the VdSwap ordinal of the guest output this present painted; 0: none painted
  uint32_t flags = 0;       // bit 0: the swap chain allows tearing (swap_chain_allows_tearing)
  int32_t present_hr = 0;   // what Present returned
  uint32_t reserved = 0;
};
static_assert(sizeof(LagPresentMark) == 32);

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
