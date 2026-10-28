/**
 * Per-controller-slot button remap for the SDL input driver.
 */

#include <rex/input/pad_remap.h>

#include <cctype>

namespace rex::input {

namespace {

struct ControlInfo {
  std::string_view name;
  uint16_t button;  // X_INPUT_GAMEPAD_* mask; 0 for the triggers
};

// Indexed by PadControl.
constexpr std::array<ControlInfo, kPadControlCount> kControls = {{
    {"A", X_INPUT_GAMEPAD_A},
    {"B", X_INPUT_GAMEPAD_B},
    {"X", X_INPUT_GAMEPAD_X},
    {"Y", X_INPUT_GAMEPAD_Y},
    {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"LT", 0},
    {"RT", 0},
    {"BACK", X_INPUT_GAMEPAD_BACK},
    {"START", X_INPUT_GAMEPAD_START},
    {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"DUP", X_INPUT_GAMEPAD_DPAD_UP},
    {"DDOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
    {"DLEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
    {"DRIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
}};

constexpr uint16_t kRemappableButtons = [] {
  uint16_t mask = 0;
  for (const auto& c : kControls) mask |= c.button;
  return mask;
}();

const ControlInfo& Info(PadControl control) {
  return kControls[static_cast<size_t>(control)];
}

bool IsTrigger(PadControl control) {
  return control == PadControl::kLT || control == PadControl::kRT;
}

uint8_t TriggerValue(const X_INPUT_GAMEPAD& pad, PadControl control) {
  return control == PadControl::kLT ? pad.left_trigger : pad.right_trigger;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

}  // namespace

std::string_view PadControlName(PadControl control) {
  return Info(control).name;
}

bool ParsePadControl(std::string_view name, PadControl* out) {
  name = Trim(name);
  for (size_t i = 0; i < kPadControlCount; ++i) {
    std::string_view candidate = kControls[i].name;
    if (candidate.size() != name.size()) continue;
    bool equal = true;
    for (size_t j = 0; j < name.size(); ++j) {
      if (std::toupper(static_cast<unsigned char>(name[j])) != candidate[j]) {
        equal = false;
        break;
      }
    }
    if (equal) {
      *out = static_cast<PadControl>(i);
      return true;
    }
  }
  return false;
}

PadRemap::PadRemap() {
  for (size_t i = 0; i < kPadControlCount; ++i) source[i] = static_cast<PadControl>(i);
}

bool PadRemap::IsIdentity() const {
  return *this == PadRemap();
}

bool ParsePadRemap(std::string_view text, PadRemap* out) {
  *out = PadRemap();
  bool all_valid = true;
  while (!text.empty()) {
    size_t comma = text.find(',');
    std::string_view item = Trim(text.substr(0, comma));
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    if (item.empty()) continue;
    size_t eq = item.find('=');
    PadControl target, source;
    if (eq == std::string_view::npos || item.find('=', eq + 1) != std::string_view::npos ||
        !ParsePadControl(item.substr(0, eq), &target) ||
        !ParsePadControl(item.substr(eq + 1), &source)) {
      all_valid = false;
      continue;
    }
    out->source[static_cast<size_t>(target)] = source;
  }
  return all_valid;
}

std::string FormatPadRemap(const PadRemap& remap) {
  std::string text;
  for (size_t i = 0; i < kPadControlCount; ++i) {
    PadControl source = remap.source[i];
    if (source == static_cast<PadControl>(i)) continue;
    if (!text.empty()) text += ',';
    text += kControls[i].name;
    text += '=';
    text += PadControlName(source);
  }
  return text;
}

void AssignPadControl(PadRemap* remap, PadControl target, PadControl source) {
  auto& sources = remap->source;
  PadControl previous = sources[static_cast<size_t>(target)];
  for (size_t i = 0; i < kPadControlCount; ++i) {
    if (sources[i] == source && static_cast<PadControl>(i) != target) {
      sources[i] = previous;
      break;
    }
  }
  sources[static_cast<size_t>(target)] = source;
}

bool IsPadControlPressed(const X_INPUT_GAMEPAD& pad, PadControl control) {
  if (IsTrigger(control)) return TriggerValue(pad, control) > kPadTriggerThreshold;
  return (static_cast<uint16_t>(pad.buttons) & Info(control).button) != 0;
}

X_INPUT_GAMEPAD ApplyPadRemap(const PadRemap& remap, const X_INPUT_GAMEPAD& raw) {
  if (remap.IsIdentity()) return raw;
  X_INPUT_GAMEPAD out = raw;
  uint16_t buttons = static_cast<uint16_t>(raw.buttons) & ~kRemappableButtons;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  for (size_t i = 0; i < kPadControlCount; ++i) {
    PadControl target = static_cast<PadControl>(i);
    PadControl source = remap.source[i];
    if (IsTrigger(target)) {
      uint8_t value = IsTrigger(source) ? TriggerValue(raw, source)
                                        : (IsPadControlPressed(raw, source) ? 0xFF : 0);
      (target == PadControl::kLT ? left_trigger : right_trigger) = value;
    } else if (IsPadControlPressed(raw, source)) {
      buttons |= Info(target).button;
    }
  }
  out.buttons = buttons;
  out.left_trigger = left_trigger;
  out.right_trigger = right_trigger;
  return out;
}

}  // namespace rex::input
