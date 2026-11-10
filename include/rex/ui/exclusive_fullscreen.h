/**
 * DXGI exclusive fullscreen (Window::SetFullscreenExclusive, Presenter::UpdateExclusiveFullscreenFromUIThread,
 * D3D12Presenter::SetExclusiveFullscreenFromUIThreadImpl): the pure decisions, so the port's host tests can pin them
 * (wt-ou1 tests/host/exclusive_mode_host_test.cpp).
 */
#pragma once

#include <cstdint>

namespace rex::ui::exclusive_fullscreen {

// What one exclusive fullscreen update did.
enum class Change {
  kUnchanged,
  kEntered,
  kLeft,
  // Entering failed for a reason a later attempt will not fix: no new attempt until the window gets the focus again,
  // is activated, or exclusive (or fullscreen) is requested again.
  kFailed,
  // DXGI left exclusive fullscreen by itself while it was still wanted: blocked like kFailed.
  kLost,
  // Entering was refused for now (the window is not the foreground window, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, a mode
  // change in progress): try again on the window's activation and on a short timer, a bounded number of times.
  kRetryLater,
};

struct Actions {
  bool resize;             // resize the swap chain's buffers for the new state
  bool block;              // no new attempt until the window's next focus gain, activation or request
  bool retry;              // schedule a bounded retry (kRetryTries at kRetryIntervalMs)
  bool restore_placement;  // put a (borderless) fullscreen window's placement back: DXGI restored the rect it saved,
                           // or the ResizeTarget before an entry that did not complete resized the window
};

constexpr Actions ActionsFor(Change change) {
  switch (change) {
    case Change::kEntered:
      return {true, false, false, false};
    case Change::kLeft:
      return {true, false, false, true};
    case Change::kLost:
      return {true, true, false, true};
    case Change::kFailed:
      return {false, true, false, true};
    case Change::kRetryLater:
      return {false, false, true, true};
    case Change::kUnchanged:
    default:
      return {false, false, false, false};
  }
}

// The retry budget of kRetryLater: tries at this interval, then the window waits for its next activation or focus.
constexpr uint32_t kRetryTries = 8;
constexpr uint32_t kRetryIntervalMs = 500;

// The outcome of SetFullscreenState(TRUE): hr as returned, in_fullscreen_after = GetFullscreenState afterwards.
// DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (0x887A0022: not the foreground window, the output is in use, the session is
// locked) and a success that did not reach fullscreen (DXGI_STATUS_MODE_CHANGE_IN_PROGRESS, 0x087A0008, and any other
// status code) are "later"; every other failure is final.
enum class EnterOutcome { kEntered, kRetryLater, kFailed };
constexpr uint32_t kHrNotCurrentlyAvailable = 0x887A0022u;
constexpr uint32_t kHrModeChangeInProgress = 0x087A0008u;
constexpr EnterOutcome ClassifyEnter(uint32_t hr, bool in_fullscreen_after) {
  if (hr & 0x80000000u) {
    return hr == kHrNotCurrentlyAvailable ? EnterOutcome::kRetryLater : EnterOutcome::kFailed;
  }
  return in_fullscreen_after ? EnterOutcome::kEntered : EnterOutcome::kRetryLater;
}

// Whether a whole-Hz refresh rate as Windows reports it (DEVMODE dmDisplayFrequency) is the rational num/den: Windows
// rounds a fractional rate either way (59.94 is reported as 59 or 60), so both the floor and the nearest count. den 0
// (an unspecified rate) cannot be checked and counts as a match.
constexpr bool RefreshMatchesWholeHz(uint32_t num, uint32_t den, uint32_t whole_hz) {
  if (den == 0) {
    return true;
  }
  const uint64_t floor_hz = uint64_t(num) / den;
  const uint64_t round_hz = (uint64_t(num) + den / 2) / den;
  return whole_hz == floor_hz || whole_hz == round_hz;
}

}  // namespace rex::ui::exclusive_fullscreen
