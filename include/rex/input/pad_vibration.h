/**
 * Per-controller-slot vibration switch.
 *
 * The cvars pad_vibration_p1 .. pad_vibration_p4 (bool, default true) turn a
 * slot's rumble off: InputSystem::SetState then hands every driver a copy with
 * both motors at zero. The drivers are still called, so the game sees the same
 * results either way, and the game's own X_INPUT_VIBRATION (guest memory) is
 * never written.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <rex/input/input.h>

namespace rex::input {

inline constexpr std::array<std::string_view, 4> kPadVibrationCvars = {
    "pad_vibration_p1", "pad_vibration_p2", "pad_vibration_p3", "pad_vibration_p4"};

// The vibration to hand the drivers for `user_index`: both motors zero when the
// slot (0-3) is switched off in `enabled`, else `requested` unchanged. Slots
// with no switch (4 and up) always get `requested`.
X_INPUT_VIBRATION VibrationToSend(uint32_t user_index, const X_INPUT_VIBRATION& requested,
                                  const std::array<bool, 4>& enabled);

}  // namespace rex::input
