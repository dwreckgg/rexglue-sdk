/**
 * Output size (Task 9c): the exclusive fullscreen mode text (the fullscreen_exclusive_mode cvar, "WxH@Hz"), the
 * monitor's mode list as the menu shows it, the choice of the mode to enter, and the windowed client sizes (the
 * window_client_size cvar, "WxH"). Pure, so the port's host tests can pin it (wt-ou1
 * tests/host/exclusive_mode_host_test.cpp). Only presentation: nothing here reaches the guest.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::ui::display_mode {

// A display mode: size in pixels and the refresh rate as DXGI gives it (a rational; den 0 = unspecified).
struct DisplayMode {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t refresh_num = 0;
  uint32_t refresh_den = 0;
};

inline double RefreshHz(const DisplayMode& m) {
  return m.refresh_den ? double(m.refresh_num) / double(m.refresh_den) : 0.0;
}

// The saved and shown text of a mode: "WxH@R", R the refresh in Hz with at most 3 decimals and no trailing zeros
// ("1920x1080@60", "1920x1080@59.94", "2560x1440@164.834"); "WxH@0" for an unspecified rate.
inline std::string ModeText(const DisplayMode& m) {
  char hz[32];
  std::snprintf(hz, sizeof(hz), "%.3f", RefreshHz(m));
  std::string r(hz);
  while (!r.empty() && r.back() == '0') r.pop_back();
  if (!r.empty() && r.back() == '.') r.pop_back();
  return std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + r;
}

// A refresh in the text matches a mode's rational within this (the text keeps 3 decimals, so 0.0005 of rounding, and
// a hand-written 59.94 against 60000/1001 = 59.94006 is 0.00006 off). Two offered modes can be this close (60000/1001
// and 59940/1000 differ by 0.00006 Hz): ChooseMode then takes the first in the monitor's list; to the monitor both are
// the same rate.
constexpr double kRefreshMatchHz = 0.001;

namespace detail {
// Parses a positive decimal integer (digits only, at most 5 of them) at the start of `s`; advances `s`.
inline bool TakeUint(std::string_view& s, uint32_t& out) {
  size_t n = 0;
  uint32_t v = 0;
  while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
    if (n == 5) return false;
    v = v * 10 + uint32_t(s[n] - '0');
    ++n;
  }
  if (n == 0 || v == 0) return false;
  s.remove_prefix(n);
  out = v;
  return true;
}
}  // namespace detail

// "WxH" -> width, height (positive, at most 5 digits each, nothing else). False for anything else, "" included.
inline bool ParseSizeText(std::string_view s, uint32_t& width, uint32_t& height) {
  uint32_t w, h;
  if (!detail::TakeUint(s, w) || s.empty() || s[0] != 'x') return false;
  s.remove_prefix(1);
  if (!detail::TakeUint(s, h) || !s.empty()) return false;
  width = w;
  height = h;
  return true;
}

// "WxH@R" -> width, height, refresh in Hz (R: digits with an optional '.' and at most 3 decimals, positive).
inline bool ParseModeText(std::string_view s, uint32_t& width, uint32_t& height, double& hz) {
  const size_t at = s.find('@');
  if (at == std::string_view::npos) return false;
  uint32_t w, h;
  if (!ParseSizeText(s.substr(0, at), w, h)) return false;
  std::string_view r = s.substr(at + 1);
  uint32_t whole = 0;
  size_t n = 0;
  while (n < r.size() && r[n] >= '0' && r[n] <= '9') {
    if (n == 4) return false;
    whole = whole * 10 + uint32_t(r[n] - '0');
    ++n;
  }
  if (n == 0) return false;
  double value = whole;
  if (n < r.size()) {
    if (r[n] != '.') return false;
    ++n;
    double scale = 0.1;
    size_t decimals = 0;
    while (n < r.size() && r[n] >= '0' && r[n] <= '9') {
      if (++decimals > 3) return false;
      value += double(r[n] - '0') * scale;
      scale /= 10.0;
      ++n;
    }
    if (decimals == 0 || n != r.size()) return false;
  }
  if (value <= 0.0) return false;
  width = w;
  height = h;
  hz = value;
  return true;
}

// The menu's list: `offered` (the monitor's modes for the swap chain's format) with the desktop mode added if it is
// missing, one entry per ModeText, largest first (width, then height, then refresh rate, all descending).
inline std::vector<DisplayMode> SortedUniqueModes(std::vector<DisplayMode> offered, const DisplayMode& desktop) {
  offered.push_back(desktop);
  std::stable_sort(offered.begin(), offered.end(), [](const DisplayMode& a, const DisplayMode& b) {
    if (a.width != b.width) return a.width > b.width;
    if (a.height != b.height) return a.height > b.height;
    return RefreshHz(a) > RefreshHz(b);
  });
  std::vector<DisplayMode> out;
  for (const DisplayMode& m : offered) {
    if (!m.width || !m.height) continue;
    if (!out.empty() && ModeText(out.back()) == ModeText(m)) continue;
    out.push_back(m);
  }
  return out;
}

enum class Pick {
  kDesktop,   // the text is "": the desktop's current mode, as asked
  kOffered,   // the text names a mode the monitor offers
  kFallback,  // the text is not a mode text, or names a mode the monitor does not offer: the desktop mode instead
};

struct Choice {
  Pick pick;
  DisplayMode mode;
  const char* reason;  // for kFallback: why (for the one log line); null otherwise
};

// The mode exclusive fullscreen enters for the saved text: "" -> the desktop mode; a text naming an offered mode (the
// same size, the refresh within kRefreshMatchHz; the first such mode in `offered`) -> that mode; anything else -> the
// desktop mode with the reason. Never an error.
inline Choice ChooseMode(std::string_view text, const std::vector<DisplayMode>& offered,
                         const DisplayMode& desktop) {
  if (text.empty()) return {Pick::kDesktop, desktop, nullptr};
  uint32_t w, h;
  double hz;
  if (!ParseModeText(text, w, h, hz)) {
    return {Pick::kFallback, desktop, "not a mode text (WxH@Hz, e.g. 1920x1080@60)"};
  }
  for (const DisplayMode& m : offered) {
    if (m.width == w && m.height == h && m.refresh_den) {
      const double d = RefreshHz(m) - hz;
      if (d <= kRefreshMatchHz && d >= -kRefreshMatchHz) return {Pick::kOffered, m, nullptr};
    }
  }
  return {Pick::kFallback, desktop, "the monitor does not offer this mode"};
}

// One axis of a window's placement inside a monitor's work area [area_begin, area_end): the start of a span of `size`
// at `pos` where it fits, else moved in (the area's begin when the span is larger than the area). The area must be in
// the coordinates `pos` is in: screen coordinates for a live window, workspace coordinates (screen minus the primary
// monitor's work-area origin) for a WINDOWPLACEMENT's rcNormalPosition, so a window on a secondary monitor stays there
// (Task 9c review M4).
constexpr int32_t PlaceInside(int32_t pos, int32_t size, int32_t area_begin, int32_t area_end) {
  return std::max(area_begin, std::min(pos, area_end - size));
}

// The windowed client sizes the menu offers: the 16:9 sizes below whose window (the client size plus the frame,
// frame_w x frame_h) fits the work area, largest first.
inline std::vector<std::pair<uint32_t, uint32_t>> FittingClientSizes(uint32_t work_w, uint32_t work_h,
                                                                     uint32_t frame_w, uint32_t frame_h) {
  static constexpr std::pair<uint32_t, uint32_t> kSizes[] = {
      {3840, 2160}, {3200, 1800}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}};
  std::vector<std::pair<uint32_t, uint32_t>> out;
  for (const auto& s : kSizes) {
    if (uint64_t(s.first) + frame_w <= work_w && uint64_t(s.second) + frame_h <= work_h) out.push_back(s);
  }
  return out;
}

}  // namespace rex::ui::display_mode
