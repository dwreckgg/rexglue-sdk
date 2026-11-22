// Host test for the D3D12 host-render-target blend factor choice
// (include/rex/graphics/d3d12/blend_factor_select.h, header-only). Built and run by
// run_blend_factor_select_test.bat; the Catch2 unit_tests target needs REXGLUE_BUILD_TESTS and a
// PPC toolchain, which this one does not. Issue #23: the 0.5 s cross-fade into a new round uses
// Xenos blend factors 14/15 (constant alpha); D3D12_BLEND_BLEND_FACTOR reads the factor's RGB, so the
// helper must hand D3D12 (A, A, A, A) for such draws.
//
// Register values decode by registers.h:779-791 (RB_BLENDCONTROL: color_srcblend bits 0-4,
// color_comb_fcn 5-7, color_destblend 8-12, alpha_srcblend 16-20, alpha_comb_fcn 21-23,
// alpha_destblend 24-28) and xenos.h:700-715 (BlendFactor: 12 kConstantColor, 13
// kOneMinusConstantColor, 14 kConstantAlpha, 15 kOneMinusConstantAlpha).

#include <rex/graphics/d3d12/blend_factor_select.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace rex::graphics;
using rex::graphics::d3d12::HostBlendColorRtMask;
using rex::graphics::d3d12::HostBlendFactorChoice;
using rex::graphics::d3d12::SelectHostBlendFactor;

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

static uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

static float FromBits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

// Bit equality of four floats (a NaN compares equal to itself here).
static bool SameBits4(const float a[4], const float b[4]) {
  return std::memcmp(a, b, sizeof(float) * 4) == 0;
}

static constexpr uint32_t kOneZero = 0x00010001;      // colour ONE/ZERO, alpha ONE/ZERO
static constexpr uint32_t kFade = 0x0F0E0F0E;         // colour 14/15, alpha 14/15
static constexpr uint32_t kConstColour = 0x0D0C0D0C;  // colour 12/13, alpha 12/13
static constexpr uint32_t kOutSentinel = 0xDEADBEEF;  // factor_out before the call

// Not 1 in RGB, so "RGB x A" and "keep RGB" both differ from (A, A, A, A).
static const float kRgba[4] = {0.5f, 0.75f, 0.125f, 0.25f};
static const float kAAAA[4] = {0.25f, 0.25f, 0.25f, 0.25f};

struct Result {
  HostBlendFactorChoice choice;
  float out[4];
};

static Result Run(uint32_t bc0, uint32_t bc1, uint32_t bc2, uint32_t bc3, uint32_t color_rt_mask,
                  bool on, const float rgba[4] = kRgba) {
  reg::RB_BLENDCONTROL bc[4];
  bc[0].value = bc0;
  bc[1].value = bc1;
  bc[2].value = bc2;
  bc[3].value = bc3;
  Result r;
  for (float& f : r.out) {
    f = FromBits(kOutSentinel);
  }
  r.choice = SelectHostBlendFactor(bc, color_rt_mask, on, rgba, r.out);
  return r;
}

static void TestFieldDecode() {
  Case("field_decode");
  reg::RB_BLENDCONTROL bc;
  bc.value = kFade;
  CHECK(bc.color_srcblend == xenos::BlendFactor::kConstantAlpha);
  CHECK(bc.color_destblend == xenos::BlendFactor::kOneMinusConstantAlpha);
  CHECK(bc.alpha_srcblend == xenos::BlendFactor::kConstantAlpha);
  CHECK(bc.alpha_destblend == xenos::BlendFactor::kOneMinusConstantAlpha);
  CHECK(uint32_t(xenos::BlendFactor::kConstantColor) == 12 &&
        uint32_t(xenos::BlendFactor::kOneMinusConstantColor) == 13 &&
        uint32_t(xenos::BlendFactor::kConstantAlpha) == 14 &&
        uint32_t(xenos::BlendFactor::kOneMinusConstantAlpha) == 15);
}

static void TestFadeDrawReplicates() {
  Case("fade_draw_replicates");
  Result r = Run(kFade, kOneZero, kOneZero, kOneZero, 0x1, true);
  CHECK(r.choice == HostBlendFactorChoice::kAlphaReplicated);
  CHECK(SameBits4(r.out, kAAAA));
}

static void TestSwitchOffKeepsRgba() {
  Case("switch_off_keeps_rgba");
  Result r = Run(kFade, kOneZero, kOneZero, kOneZero, 0x1, false);
  CHECK(r.choice == HostBlendFactorChoice::kRgba);
  CHECK(SameBits4(r.out, kRgba));
}

static void TestConstantColourKeepsRgba() {
  Case("constant_colour_keeps_rgba");
  Result r = Run(kConstColour, kOneZero, kOneZero, kOneZero, 0x1, true);
  CHECK(r.choice == HostBlendFactorChoice::kRgba);
  CHECK(SameBits4(r.out, kRgba));
}

static void TestMixedSameTarget() {
  Case("mixed_same_target");
  // Colour src 12 (constant colour), dest 15 (one minus constant alpha); alpha 14/15.
  Result on = Run(0x0F0E0F0C, kOneZero, kOneZero, kOneZero, 0x1, true);
  CHECK(on.choice == HostBlendFactorChoice::kMixed);
  CHECK(SameBits4(on.out, kRgba));
  Result off = Run(0x0F0E0F0C, kOneZero, kOneZero, kOneZero, 0x1, false);
  CHECK(off.choice == HostBlendFactorChoice::kMixed);
  CHECK(SameBits4(off.out, kRgba));
}

static void TestMixedAcrossTargets() {
  Case("mixed_across_targets");
  Result r = Run(kFade, kConstColour, kOneZero, kOneZero, 0x3, true);
  CHECK(r.choice == HostBlendFactorChoice::kMixed);
  CHECK(SameBits4(r.out, kRgba));
}

static void TestMaskedTargetIgnored() {
  Case("masked_target_ignored");
  Result r = Run(kFade, kConstColour, kOneZero, kOneZero, 0x1, true);
  CHECK(r.choice == HostBlendFactorChoice::kAlphaReplicated);
  CHECK(SameBits4(r.out, kAAAA));
}

static void TestAlphaSlotConstantColourIgnored() {
  Case("alpha_slot_constant_colour_ignored");
  // Colour 14/15, alpha 12/13: only the colour factors are read (D1).
  Result r = Run(0x0D0C0F0E, kOneZero, kOneZero, kOneZero, 0x1, true);
  CHECK(r.choice == HostBlendFactorChoice::kAlphaReplicated);
  CHECK(SameBits4(r.out, kAAAA));
}

static void TestAlphaSlotConstantAlphaOnly() {
  Case("alpha_slot_constant_alpha_only");
  // Colour ONE/ZERO, alpha 14/15: the alpha channel reads A from (R, G, B, A) already.
  Result r = Run(0x0F0E0001, kOneZero, kOneZero, kOneZero, 0x1, true);
  CHECK(r.choice == HostBlendFactorChoice::kRgba);
  CHECK(SameBits4(r.out, kRgba));
}

static void TestNoTargets() {
  Case("no_targets");
  Result r = Run(kFade, kOneZero, kOneZero, kOneZero, 0x0, true);
  CHECK(r.choice == HostBlendFactorChoice::kRgba);
  CHECK(SameBits4(r.out, kRgba));
}

static void TestFourthTargetCounts() {
  Case("fourth_target_counts");
  Result r = Run(kOneZero, kOneZero, kOneZero, kFade, 0x8, true);
  CHECK(r.choice == HostBlendFactorChoice::kAlphaReplicated);
  CHECK(SameBits4(r.out, kAAAA));
}

static void TestNanAlphaBitCopied() {
  Case("nan_alpha_bit_copied");
  const float one = FromBits(0x3F800000);
  const float snan = FromBits(0x7F800001);
  const float rgba[4] = {one, one, one, snan};
  Result r = Run(kFade, kOneZero, kOneZero, kOneZero, 0x1, true, rgba);
  CHECK(r.choice == HostBlendFactorChoice::kAlphaReplicated);
  CHECK(Bits(r.out[0]) == 0x7F800001);
  CHECK(Bits(r.out[1]) == 0x7F800001);
  CHECK(Bits(r.out[2]) == 0x7F800001);
  CHECK(Bits(r.out[3]) == 0x7F800001);
}

static void TestRtMask() {
  Case("rt_mask_depth_only");
  CHECK(HostBlendColorRtMask(0xFFFF, 0x1) == 0);
  Case("rt_mask_rt0");
  CHECK(HostBlendColorRtMask(0x000F, 0x2) == 0x1);
  Case("rt_mask_rt0_masked");
  CHECK(HostBlendColorRtMask(0x0000, 0x2) == 0);
  Case("rt_mask_rt3");
  CHECK(HostBlendColorRtMask(0xF000, 0x10) == 0x8);
  Case("rt_mask_unbound");
  CHECK(HostBlendColorRtMask(0x00F0, 0x2) == 0);
}

int main() {
  TestFieldDecode();
  TestFadeDrawReplicates();
  TestSwitchOffKeepsRgba();
  TestConstantColourKeepsRgba();
  TestMixedSameTarget();
  TestMixedAcrossTargets();
  TestMaskedTargetIgnored();
  TestAlphaSlotConstantColourIgnored();
  TestAlphaSlotConstantAlphaOnly();
  TestNoTargets();
  TestFourthTargetCounts();
  TestNanAlphaBitCopied();
  TestRtMask();
  if (g_failures != 0) {
    std::printf("%d of %d check(s) failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("blend_factor_select: %d checks, all passed\n", g_checks);
  return 0;
}
