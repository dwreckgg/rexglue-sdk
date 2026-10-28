/**
 * Per-controller-slot button remap for the SDL input driver.
 *
 * A remap says, for each remappable Xbox control (the target), which physical
 * control drives it (the source). It is stored as text in the cvars
 * pad_remap_p1 .. pad_remap_p4, e.g. "A=B,B=A,LT=RB" (target=source pairs);
 * controls not listed drive themselves. Analog sticks are not remappable.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/input/input.h>

namespace rex::input {

// The remappable controls, in display and serialisation order.
enum class PadControl : uint8_t {
  kA,
  kB,
  kX,
  kY,
  kLB,
  kRB,
  kLT,
  kRT,
  kBack,
  kStart,
  kLS,
  kRS,
  kDUp,
  kDDown,
  kDLeft,
  kDRight,
};
inline constexpr size_t kPadControlCount = 16;

// A trigger counts as pressed above this value (the SDL driver's
// HID_SDL_TRIGG_THRES; sdl_input_driver.cpp static_asserts they match).
inline constexpr uint8_t kPadTriggerThreshold = 0x1F;

// The token used in pad_remap_pN text: A B X Y LB RB LT RT BACK START LS RS
// DUP DDOWN DLEFT DRIGHT.
std::string_view PadControlName(PadControl control);

// Case-insensitive inverse of PadControlName. Returns false if unknown.
bool ParsePadControl(std::string_view name, PadControl* out);

struct PadRemap {
  // source[target] = the physical control that drives `target`.
  std::array<PadControl, kPadControlCount> source;

  PadRemap();  // identity
  bool IsIdentity() const;
  bool operator==(const PadRemap&) const = default;
};

// Parses "T=S,T=S,..." on top of identity. Spaces are ignored, names are
// case-insensitive, empty items are skipped, a later pair for the same target
// wins. Unknown names and malformed items are skipped; the function then
// returns false, and every valid pair still applies.
bool ParsePadRemap(std::string_view text, PadRemap* out);

// The non-identity pairs in PadControl order, e.g. "A=B,B=A"; "" for
// identity. Round-trips with ParsePadRemap.
std::string FormatPadRemap(const PadRemap& remap);

// Makes `source` drive `target`. The control `source` drove before gets the
// source `target` had, so each physical control keeps driving exactly one
// control (a swap).
void AssignPadControl(PadRemap* remap, PadControl target, PadControl source);

// Whether `control` is pressed in `pad` (triggers: above kPadTriggerThreshold).
bool IsPadControlPressed(const X_INPUT_GAMEPAD& pad, PadControl control);

// The gamepad the guest sees: each target reads its source. A trigger source
// drives a button target when above the threshold; a button source drives a
// trigger target as 255 / 0; trigger to trigger copies the analog value.
// Sticks and the buttons that are not remappable (Guide) pass through.
X_INPUT_GAMEPAD ApplyPadRemap(const PadRemap& remap, const X_INPUT_GAMEPAD& raw);

}  // namespace rex::input
