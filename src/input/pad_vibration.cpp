#include <rex/input/pad_vibration.h>

namespace rex::input {

X_INPUT_VIBRATION VibrationToSend(uint32_t user_index, const X_INPUT_VIBRATION& requested,
                                  const std::array<bool, 4>& enabled) {
  if (user_index < enabled.size() && !enabled[user_index]) {
    X_INPUT_VIBRATION quiet;
    quiet.left_motor_speed = uint16_t(0);
    quiet.right_motor_speed = uint16_t(0);
    return quiet;
  }
  return requested;
}

}  // namespace rex::input
