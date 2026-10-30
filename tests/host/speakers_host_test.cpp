// Host test for the speaker layout choice (rex/audio/speaker_mode.h) and the
// 5.1 -> stereo / 5.1 -> 6-channel conversions (rex/audio/conversion.h).
// Built and run by run_speakers_test.bat.

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/speaker_mode.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace rex::audio;

static float SwapFloat(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  u = (u >> 24) | ((u >> 8) & 0xFF00u) | ((u << 8) & 0xFF0000u) | (u << 24);
  std::memcpy(&v, &u, 4);
  return v;
}

constexpr size_t kSamples = 8;  // the SSE version needs a multiple of 4
// Xbox 360 / XAudio2 channel order: FL FR C LFE BL BR. A power of two per
// channel, so any wrong pairing of channels gives a different sum.
constexpr float kChannelValue[6] = {1.f, 2.f, 4.f, 8.f, 16.f, 32.f};

// Planar, big-endian, as the guest writes it. Sample s of every channel is
// scaled by (s + 1), so a mix-up between samples shows as well.
static std::vector<float> GuestFrame() {
  std::vector<float> in(6 * kSamples);
  for (size_t c = 0; c < 6; ++c) {
    for (size_t s = 0; s < kSamples; ++s) {
      in[c * kSamples + s] = SwapFloat(kChannelValue[c] * float(s + 1));
    }
  }
  return in;
}

static bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-4f;
}

using Convert2 = void (*)(float*, const float*, size_t);

static void CheckStereo(Convert2 convert, const char* name) {
  std::vector<float> in = GuestFrame();
  std::vector<float> out(2 * kSamples, -1.f);
  convert(out.data(), in.data(), kSamples);
  // left = (FL + BL + C/2) / 2.5 = (1 + 16 + 2) / 2.5; right = (FR + BR + C/2) / 2.5.
  // A back-channel swap would give left (1 + 32 + 2) / 2.5 = 14. LFE (8) is dropped.
  bool ok = true;
  for (size_t s = 0; s < kSamples; ++s) {
    const float k = float(s + 1);
    ok = ok && Near(out[s * 2], 7.6f * k) && Near(out[s * 2 + 1], 14.4f * k);
  }
  if (!ok) {
    std::printf("stereo %s: got L=%g R=%g for sample 0, want 7.6 14.4\n", name, out[0], out[1]);
  }
  CHECK(ok);
}

static void CheckSix(Convert2 convert, const char* name) {
  std::vector<float> in = GuestFrame();
  std::vector<float> out(6 * kSamples, -1.f);
  convert(out.data(), in.data(), kSamples);
  bool ok = true;
  for (size_t s = 0; s < kSamples; ++s) {
    for (size_t c = 0; c < 6; ++c) {
      ok = ok && Near(out[s * 6 + c], kChannelValue[c] * float(s + 1));
    }
  }
  if (!ok) std::printf("six-channel %s: interleave or order wrong\n", name);
  CHECK(ok);
}

static void TestConversions() {
  CheckStereo(conversion::sequential_6_BE_to_interleaved_2_LE, "default");
  CheckStereo(conversion::sequential_6_BE_to_interleaved_2_LE_scalar, "scalar");
  CheckSix(conversion::sequential_6_BE_to_interleaved_6_LE, "default");
  CheckSix(conversion::sequential_6_BE_to_interleaved_6_LE_scalar, "scalar");

  // The fast and the portable stereo versions agree on arbitrary data.
  std::vector<float> in(6 * kSamples);
  for (size_t i = 0; i < in.size(); ++i) in[i] = SwapFloat(std::sin(float(i) * 0.37f) * 0.8f);
  std::vector<float> a(2 * kSamples), b(2 * kSamples);
  conversion::sequential_6_BE_to_interleaved_2_LE(a.data(), in.data(), kSamples);
  conversion::sequential_6_BE_to_interleaved_2_LE_scalar(b.data(), in.data(), kSamples);
  bool same = true;
  for (size_t i = 0; i < a.size(); ++i) same = same && Near(a[i], b[i]);
  CHECK(same);
}

static void TestSpeakerMode() {
  CHECK(ParseSpeakerMode("auto") == SpeakerMode::kAuto);
  CHECK(ParseSpeakerMode("stereo") == SpeakerMode::kStereo);
  CHECK(ParseSpeakerMode("surround") == SpeakerMode::kSurround);
  CHECK(ParseSpeakerMode("") == SpeakerMode::kAuto);
  CHECK(ParseSpeakerMode("Surround") == SpeakerMode::kAuto);  // case-sensitive; the cvar's list is lower case
  CHECK(ParseSpeakerMode("5.1") == SpeakerMode::kAuto);

  // Device 2 / 6 / 8 / unknown (0) under each mode.
  CHECK(ChooseStreamChannels(SpeakerMode::kAuto, 2) == 2);
  CHECK(ChooseStreamChannels(SpeakerMode::kAuto, 6) == 6);
  CHECK(ChooseStreamChannels(SpeakerMode::kAuto, 8) == 6);
  CHECK(ChooseStreamChannels(SpeakerMode::kAuto, 0) == 6);
  CHECK(ChooseStreamChannels(SpeakerMode::kStereo, 2) == 2);
  CHECK(ChooseStreamChannels(SpeakerMode::kStereo, 6) == 2);  // forced downmix on a 5.1 device
  CHECK(ChooseStreamChannels(SpeakerMode::kStereo, 8) == 2);
  CHECK(ChooseStreamChannels(SpeakerMode::kSurround, 2) == 6);  // forced 6 on a stereo device
  CHECK(ChooseStreamChannels(SpeakerMode::kSurround, 6) == 6);
  CHECK(ChooseStreamChannels(SpeakerMode::kSurround, 8) == 6);
}

int main() {
  TestConversions();
  TestSpeakerMode();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all speakers checks passed\n");
  return 0;
}
