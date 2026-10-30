// Speaker layout choice for the SDL audio driver, plus a read-out of what the
// output device actually gave. No SDL headers, so a host test can build it.

#pragma once

#include <cstdint>
#include <string_view>

namespace rex::audio {

enum class SpeakerMode : uint8_t {
  kAuto,      // follow the device: 6 channels unless the device is stereo
  kStereo,    // always the stereo downmix
  kSurround,  // always 6 channels; SDL converts if the device is really stereo
};

// "auto" | "stereo" | "surround"; anything else is kAuto.
inline SpeakerMode ParseSpeakerMode(std::string_view text) {
  if (text == "stereo") return SpeakerMode::kStereo;
  if (text == "surround") return SpeakerMode::kSurround;
  return SpeakerMode::kAuto;
}

// Channels of the stream the driver feeds SDL (2 or 6), given the device's own
// channel count.
inline uint8_t ChooseStreamChannels(SpeakerMode mode, uint32_t device_channels) {
  switch (mode) {
    case SpeakerMode::kStereo:
      return 2;
    case SpeakerMode::kSurround:
      return 6;
    case SpeakerMode::kAuto:
    default:
      return device_channels == 2 ? 2 : 6;
  }
}

struct OutputInfo {
  uint8_t device_channels = 0;  // what the Windows default device reports; 0 = not opened yet
  uint8_t stream_channels = 0;  // what the driver feeds SDL
};

// Written by the SDL driver at start-up, read by the port's menu.
void SetOutputInfo(const OutputInfo& info);
OutputInfo GetOutputInfo();

}  // namespace rex::audio
