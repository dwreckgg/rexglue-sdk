/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xnotifylistener.h>

namespace rex::system {

namespace {
// Netplay lockstep (rex_notify_lockstep): a notification is numbered when it is queued and becomes
// visible to the guest at the first frame boundary after that (rex_notify_frame), so every run of a
// session dequeues it in the same frame; input-device notifications (the host's own controllers; a
// session's input is staged) are dropped.
std::atomic<bool> g_notify_lockstep{false};
std::atomic<uint64_t> g_notify_seq{0};      // the next notification's number
std::atomic<uint64_t> g_notify_visible{0};  // notifications numbered below this are visible
std::atomic<uint32_t> g_notify_delivered{0};

// Input-device notifications are 0x12 (InputDevicesChanged) and 0x13 (InputDeviceConfigChanged). This SDK
// posts them only in the canned startup burst of the first system listener (KernelState::
// RegisterNotifyListener, two of each, the same on every PC); no call site posts one when a host
// controller is plugged in or removed (BroadcastNotification is called only from xam_ui.cpp: 0x9,
// xam_user.cpp: 0xA and 0x9, xmp_app.cpp: media player ids). So there is no notification about the host's
// own controllers to drop, and dropping the startup pair would only make the session differ from offline
// play: nothing is dropped. A future call site that posts one on a host controller change adds its id here.
bool DroppedInLockstep(XNotificationID id) {
  (void)id;
  return false;
}
}  // namespace

XNotifyListener::XNotifyListener(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XNotifyListener::~XNotifyListener() {}

void XNotifyListener::Initialize(uint64_t mask, uint32_t max_version) {
  assert_false(wait_handle_);

  wait_handle_ = rex::thread::Event::CreateManualResetEvent(false);
  assert_not_null(wait_handle_);
  mask_ = mask;
  max_version_ = max_version;

  kernel_state_->RegisterNotifyListener(this);
}

void XNotifyListener::EnqueueNotification(XNotificationID id, uint32_t data) {
  auto key = XNotificationKey(id);
  // Ignore if the notification doesn't match our mask.
  if ((mask_ & uint64_t(1ULL << key.mask_index)) == 0) {
    return;
  }
  // Ignore if the notification is too new.
  if (key.version > max_version_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  notifications_.push_back(std::pair<XNotificationID, uint32_t>(id, data));
  seqs_.push_back(g_notify_seq.fetch_add(1, std::memory_order_acq_rel));
  wait_handle_->Set();
}

bool XNotifyListener::DequeueNotification(XNotificationID* out_id, uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  const bool lockstep = g_notify_lockstep.load(std::memory_order_acquire);
  const uint64_t visible = g_notify_visible.load(std::memory_order_acquire);
  for (size_t i = 0; i < notifications_.size();) {
    if (lockstep && seqs_[i] >= visible) break;  // queued after the last boundary (so are the rest)
    if (lockstep && DroppedInLockstep(notifications_[i].first)) {
      notifications_.erase(notifications_.begin() + i);
      seqs_.erase(seqs_.begin() + i);
      continue;
    }
    *out_id = notifications_[i].first;
    *out_data = notifications_[i].second;
    notifications_.erase(notifications_.begin() + i);
    seqs_.erase(seqs_.begin() + i);
    if (lockstep) g_notify_delivered.fetch_add(1, std::memory_order_relaxed);
    if (notifications_.empty()) wait_handle_->Reset();
    return true;
  }
  if (notifications_.empty()) wait_handle_->Reset();
  return false;
}

bool XNotifyListener::DequeueNotification(XNotificationID id, uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  const bool lockstep = g_notify_lockstep.load(std::memory_order_acquire);
  const uint64_t visible = g_notify_visible.load(std::memory_order_acquire);
  for (size_t i = 0; i < notifications_.size(); ++i) {
    if (lockstep && seqs_[i] >= visible) break;
    if (notifications_[i].first != id || (lockstep && DroppedInLockstep(id))) continue;
    *out_data = notifications_[i].second;
    notifications_.erase(notifications_.begin() + i);
    seqs_.erase(seqs_.begin() + i);
    if (lockstep) g_notify_delivered.fetch_add(1, std::memory_order_relaxed);
    if (notifications_.empty()) wait_handle_->Reset();
    return true;
  }
  return false;
}

bool XNotifyListener::Save(stream::ByteStream* stream) {
  SaveObject(stream);

  stream->Write(mask_);
  stream->Write(max_version_);
  stream->Write(notifications_.size());
  for (auto pair : notifications_) {
    stream->Write<uint32_t>(pair.first);
    stream->Write<uint32_t>(pair.second);
  }

  return true;
}

object_ref<XNotifyListener> XNotifyListener::Restore(KernelState* kernel_state,
                                                     stream::ByteStream* stream) {
  auto notify = new XNotifyListener(nullptr);
  notify->kernel_state_ = kernel_state;

  notify->RestoreObject(stream);

  auto mask = stream->Read<uint64_t>();
  auto max_version = stream->Read<uint32_t>();
  notify->Initialize(mask, max_version);

  auto notification_count_ = stream->Read<size_t>();
  for (size_t i = 0; i < notification_count_; i++) {
    std::pair<XNotificationID, uint32_t> pair;
    pair.first = stream->Read<uint32_t>();
    pair.second = stream->Read<uint32_t>();
    notify->notifications_.push_back(pair);
  }
  notify->seqs_.resize(notify->notifications_.size(), 0);  // netplay: the two vectors stay parallel

  return object_ref<XNotifyListener>(notify);
}

}  // namespace rex::system

// Netplay: on = 1 starts the lockstep (everything queued so far is visible), 0 ends it.
extern "C" __declspec(dllexport) void rex_notify_lockstep(int on) {
  rex::system::g_notify_visible.store(rex::system::g_notify_seq.load());
  rex::system::g_notify_lockstep.store(on != 0, std::memory_order_release);
}
// Netplay: a frame boundary - everything queued so far becomes visible.
extern "C" __declspec(dllexport) void rex_notify_frame() {
  rex::system::g_notify_visible.store(rex::system::g_notify_seq.load(), std::memory_order_release);
}
// Netplay: notifications the guest dequeued in the lockstep.
extern "C" __declspec(dllexport) uint32_t rex_notify_delivered() {
  return rex::system::g_notify_delivered.load(std::memory_order_relaxed);
}
