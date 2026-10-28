// Host test for rex::input pad remapping (src/input/pad_remap.cpp).
// Built and run by run_pad_remap_test.bat; the Catch2 unit_tests target needs
// REXGLUE_BUILD_TESTS and a PPC toolchain, which this one does not.

#include <rex/input/pad_remap.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace rex::input;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static PadRemap Parse(const char* text, bool* ok = nullptr) {
  PadRemap remap;
  bool result = ParsePadRemap(text, &remap);
  if (ok) *ok = result;
  return remap;
}

static X_INPUT_GAMEPAD Pad(uint16_t buttons, uint8_t lt = 0, uint8_t rt = 0) {
  X_INPUT_GAMEPAD pad;
  std::memset(&pad, 0, sizeof(pad));
  pad.buttons = buttons;
  pad.left_trigger = lt;
  pad.right_trigger = rt;
  return pad;
}

static uint16_t Buttons(const X_INPUT_GAMEPAD& pad) {
  return static_cast<uint16_t>(pad.buttons);
}

// Every physical control drives exactly one control.
static bool IsPermutation(const PadRemap& remap) {
  bool seen[kPadControlCount] = {};
  for (PadControl source : remap.source) {
    size_t i = static_cast<size_t>(source);
    if (seen[i]) return false;
    seen[i] = true;
  }
  return true;
}

static void TestNames() {
  PadControl c;
  CHECK(ParsePadControl("dright", &c) && c == PadControl::kDRight);
  CHECK(ParsePadControl(" Start ", &c) && c == PadControl::kStart);
  CHECK(!ParsePadControl("DPAD", &c));
  CHECK(PadControlName(PadControl::kLB) == "LB");
}

static void TestParseFormat() {
  bool ok = false;
  PadRemap swap = Parse("A=B,B=A", &ok);
  CHECK(ok);
  CHECK(swap.source[static_cast<size_t>(PadControl::kA)] == PadControl::kB);
  CHECK(swap.source[static_cast<size_t>(PadControl::kB)] == PadControl::kA);
  CHECK(FormatPadRemap(swap) == "A=B,B=A");
  // Case, spaces, order and empty items do not matter; output is in control order.
  CHECK(FormatPadRemap(Parse(" b = a ,, a=B ,")) == "A=B,B=A");
  // A later pair for the same target wins.
  CHECK(FormatPadRemap(Parse("A=X,A=Y")) == "A=Y");
  // Round trip.
  PadRemap again;
  CHECK(ParsePadRemap(FormatPadRemap(swap), &again) && again == swap);
  // Identity.
  CHECK(Parse("").IsIdentity());
  CHECK(FormatPadRemap(PadRemap()).empty());
  CHECK(FormatPadRemap(Parse("A=A")).empty());
}

static void TestBadPairsSkipped() {
  bool ok = true;
  PadRemap remap = Parse("A=B,Q=A,B,=X,X=Y=Z,B=A", &ok);
  CHECK(!ok);
  CHECK(FormatPadRemap(remap) == "A=B,B=A");
}

static void TestAssignSwaps() {
  PadRemap remap;
  AssignPadControl(&remap, PadControl::kA, PadControl::kB);
  CHECK(FormatPadRemap(remap) == "A=B,B=A");
  // B drove A; X takes B, so A gets X's old source (X).
  AssignPadControl(&remap, PadControl::kX, PadControl::kB);
  CHECK(FormatPadRemap(remap) == "A=X,B=A,X=B");
  CHECK(IsPermutation(remap));
  // Assigning a control its current source changes nothing.
  PadRemap before = remap;
  AssignPadControl(&remap, PadControl::kX, PadControl::kB);
  CHECK(remap == before);
  // Undo back to identity.
  AssignPadControl(&remap, PadControl::kX, PadControl::kX);
  AssignPadControl(&remap, PadControl::kA, PadControl::kA);
  CHECK(remap.IsIdentity());
  // Triggers and buttons swap the same way.
  AssignPadControl(&remap, PadControl::kLT, PadControl::kRB);
  CHECK(FormatPadRemap(remap) == "RB=LT,LT=RB");
  CHECK(IsPermutation(remap));
}

static void TestApplyButtons() {
  PadRemap swap = Parse("A=B,B=A");
  CHECK(Buttons(ApplyPadRemap(swap, Pad(X_INPUT_GAMEPAD_A))) == X_INPUT_GAMEPAD_B);
  CHECK(Buttons(ApplyPadRemap(swap, Pad(X_INPUT_GAMEPAD_B))) == X_INPUT_GAMEPAD_A);
  // A hand-edited, non-permutation remap: B drives both A and B, and A's own
  // press drives nothing (an implementation that ORs the raw buttons in fails).
  PadRemap shared = Parse("A=B");
  CHECK(Buttons(ApplyPadRemap(shared, Pad(X_INPUT_GAMEPAD_B))) ==
        (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
  CHECK(Buttons(ApplyPadRemap(shared, Pad(X_INPUT_GAMEPAD_A))) == 0);
}

static void TestApplyTriggers() {
  // Button source -> trigger target: 255 / 0.
  PadRemap lt_from_a = Parse("LT=A,A=LT");
  X_INPUT_GAMEPAD out = ApplyPadRemap(lt_from_a, Pad(X_INPUT_GAMEPAD_A));
  CHECK(out.left_trigger == 0xFF);
  CHECK(Buttons(out) == 0);
  CHECK(ApplyPadRemap(lt_from_a, Pad(0)).left_trigger == 0);
  // Trigger source -> button target, both sides of the threshold.
  CHECK(Buttons(ApplyPadRemap(lt_from_a, Pad(0, kPadTriggerThreshold))) == 0);
  CHECK(Buttons(ApplyPadRemap(lt_from_a, Pad(0, kPadTriggerThreshold + 1))) ==
        X_INPUT_GAMEPAD_A);
  // Trigger -> trigger copies the analog value.
  X_INPUT_GAMEPAD swapped = ApplyPadRemap(Parse("LT=RT,RT=LT"), Pad(0, 10, 200));
  CHECK(swapped.left_trigger == 200);
  CHECK(swapped.right_trigger == 10);
}

static void TestPassThrough() {
  X_INPUT_GAMEPAD raw = Pad(X_INPUT_GAMEPAD_GUIDE | X_INPUT_GAMEPAD_Y, 40, 50);
  raw.thumb_lx = -1234;
  raw.thumb_ly = 2345;
  raw.thumb_rx = -32768;
  raw.thumb_ry = 32767;
  // Identity: byte-identical.
  X_INPUT_GAMEPAD same = ApplyPadRemap(PadRemap(), raw);
  CHECK(std::memcmp(&same, &raw, sizeof(raw)) == 0);
  // Non-identity: sticks and Guide unchanged.
  X_INPUT_GAMEPAD out = ApplyPadRemap(Parse("A=B,B=A"), raw);
  CHECK(static_cast<int16_t>(out.thumb_lx) == -1234);
  CHECK(static_cast<int16_t>(out.thumb_ly) == 2345);
  CHECK(static_cast<int16_t>(out.thumb_rx) == -32768);
  CHECK(static_cast<int16_t>(out.thumb_ry) == 32767);
  CHECK(Buttons(out) == (X_INPUT_GAMEPAD_GUIDE | X_INPUT_GAMEPAD_Y));
  CHECK(out.left_trigger == 40 && out.right_trigger == 50);
}

int main() {
  TestNames();
  TestParseFormat();
  TestBadPairsSkipped();
  TestAssignSwaps();
  TestApplyButtons();
  TestApplyTriggers();
  TestPassThrough();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("pad_remap: all checks passed\n");
  return 0;
}
