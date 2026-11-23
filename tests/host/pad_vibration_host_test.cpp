// Host test for rex::input's per-slot vibration switch (src/input/pad_vibration.cpp).
// Built and run by run_pad_vibration_test.bat (clang-cl, no CMake), like the pad remap test.

#include <rex/input/pad_vibration.h>

#include <array>
#include <cstdint>
#include <cstdio>

using namespace rex::input;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

// Both motors non-zero and different, so a mutant that zeroes or swaps one motor is caught.
static X_INPUT_VIBRATION Requested() {
  X_INPUT_VIBRATION v;
  v.left_motor_speed = uint16_t(0x1234);
  v.right_motor_speed = uint16_t(0xABCD);
  return v;
}

static uint16_t Left(const X_INPUT_VIBRATION& v) { return static_cast<uint16_t>(v.left_motor_speed); }
static uint16_t Right(const X_INPUT_VIBRATION& v) { return static_cast<uint16_t>(v.right_motor_speed); }
static bool IsRequested(const X_INPUT_VIBRATION& v) { return Left(v) == 0x1234 && Right(v) == 0xABCD; }
static bool IsQuiet(const X_INPUT_VIBRATION& v) { return Left(v) == 0 && Right(v) == 0; }

int main() {
  const X_INPUT_VIBRATION req = Requested();

  // the cvar names, in slot order
  CHECK(kPadVibrationCvars.size() == 4);
  CHECK(kPadVibrationCvars[0] == "pad_vibration_p1");
  CHECK(kPadVibrationCvars[1] == "pad_vibration_p2");
  CHECK(kPadVibrationCvars[2] == "pad_vibration_p3");
  CHECK(kPadVibrationCvars[3] == "pad_vibration_p4");

  // all on: every slot gets the request
  {
    const std::array<bool, 4> on = {true, true, true, true};
    for (uint32_t slot = 0; slot < 4; ++slot) CHECK(IsRequested(VibrationToSend(slot, req, on)));
  }
  // slot 2 off: only slot 2 is quiet
  {
    const std::array<bool, 4> en = {true, true, false, true};
    CHECK(IsQuiet(VibrationToSend(2, req, en)));
    CHECK(IsRequested(VibrationToSend(0, req, en)));
    CHECK(IsRequested(VibrationToSend(1, req, en)));
    CHECK(IsRequested(VibrationToSend(3, req, en)));
  }
  // slot 0 off: slot 0 quiet, slot 1 not (an off-by-one index fails here or above)
  {
    const std::array<bool, 4> en = {false, true, true, true};
    CHECK(IsQuiet(VibrationToSend(0, req, en)));
    CHECK(IsRequested(VibrationToSend(1, req, en)));
    CHECK(IsRequested(VibrationToSend(2, req, en)));
  }
  // slots with no switch (4 and up, 0xFF) pass the request through even with every switch off
  {
    const std::array<bool, 4> off = {false, false, false, false};
    CHECK(IsRequested(VibrationToSend(4, req, off)));
    CHECK(IsRequested(VibrationToSend(0xFF, req, off)));
    for (uint32_t slot = 0; slot < 4; ++slot) CHECK(IsQuiet(VibrationToSend(slot, req, off)));
  }
  // the request itself is never written
  CHECK(IsRequested(req));

  if (g_failures) {
    std::printf("pad_vibration: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("pad_vibration: all checks passed\n");
  return 0;
}
