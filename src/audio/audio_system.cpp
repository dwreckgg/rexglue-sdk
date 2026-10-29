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

#include <condition_variable>
#include <mutex>
#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 8, "Audio",
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace rex::audio {

extern std::atomic<bool> g_xma_lockstep;  // xma_decoder.cpp

namespace {
// Netplay audio lockstep (Tekken 6 rollback): the game asks the sound library whether a sound has finished and its
// round intro waits for that, so the guest audio callbacks (each renders one 256-sample frame and retires finished
// voices) must advance with the game frames, not with the host audio clock. While enabled, the worker runs a callback
// only within the budget that rex_audio_advance gives at every game frame boundary.
// (Ported from the Tekken 6 SDK patch to v0.8: the hunk's anchor kWorkerShutdownTimeout does not exist here.)
std::atomic<bool> g_audio_lockstep{false};
std::atomic<uint64_t> g_audio_budget{0}, g_audio_done{0};
std::atomic<uint64_t> g_audio_lock_base{0};    // g_audio_done when the lockstep was (last) switched on
std::atomic<void*> g_audio_cb_ctx{nullptr};    // guest context of the worker while a callback runs (sampler)
std::atomic<uint32_t> g_audio_worker_tid{0};   // host thread id of the worker (runs the guest callbacks)
std::atomic<uint32_t> g_audio_registrations{0};  // clients the guest registered (netplay: rex_audio_registrations)
std::mutex g_audio_mutex;
std::condition_variable g_audio_cv;
}  // namespace

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(4)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();
  g_audio_worker_tid = rex::thread::current_thread_system_id();
  REXAPU_INFO("AudioWorker: host thread {}", g_audio_worker_tid.load());

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
    if (g_audio_lockstep) {
      std::unique_lock lock(g_audio_mutex);
      g_audio_cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return !g_audio_lockstep || g_audio_done < g_audio_budget || !worker_running_;
      });
      if (g_audio_lockstep && g_audio_done >= g_audio_budget) continue;
    }
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result = rex::thread::WaitAny(wait_handles_, rex::countof(wait_handles_), true,
                                       std::chrono::milliseconds(500));
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        thread::Wait(resume_event_.get(), false);
      }

      continue;
    }

    // Netplay: the budget gate at the top of the loop is checked before WaitAny. The lockstep can be switched on
    // while this worker is already inside WaitAny, and a client registered meanwhile releases its semaphore and
    // wakes it with no budget: callback 0 would then run at a host-timed point, in the middle of the guest's init.
    // So a callback also waits for its budget here (the semaphore count WaitAny took is held until then).
    // Not in the lockstep: unchanged.
    if (result.first == thread::WaitResult::kSuccess && g_audio_lockstep) {
      std::unique_lock lock(g_audio_mutex);
      while (!g_audio_cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return !g_audio_lockstep || g_audio_done < g_audio_budget || !worker_running_;
      })) {
      }
      if (!worker_running_) break;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();

      if (client_callback) {
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        g_audio_cb_ctx.store(worker_thread_->thread_state()->context(), std::memory_order_relaxed);
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        g_audio_cb_ctx.store(nullptr, std::memory_order_relaxed);
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
        {
          std::lock_guard lock(g_audio_mutex);
          g_audio_done++;
        }
        g_audio_cv.notify_all();
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      // Netplay: the 500 ms pause after a WaitAny timeout is a wait on the lockstep condition variable, ended at
      // once when the lockstep is armed (rex_audio_lockstep(1) notifies it) or the worker is stopping. A plain
      // Sleep would hold the worker for the rest of the 500 ms when the lockstep is armed during it, while the
      // game's frame boundary waits for callbacks; the gate at the top of the loop then does the waiting.
      // Offline nothing makes the predicate true, so it stays a 500 ms pause (callback notifications do not end it).
      std::unique_lock lock(g_audio_mutex);
      g_audio_cv.wait_for(lock, std::chrono::milliseconds(500),
                          [this] { return g_audio_lockstep || !worker_running_; });
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked
    // on guest objects (e.g. KeWaitForMultipleObjects).
    // Terminate the thread to break the deadlock.
    worker_thread_->Terminate(0);
    worker_thread_.reset();
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};
  g_audio_registrations.fetch_add(1, std::memory_order_release);

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_ERROR(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio

// Netplay audio lockstep (see g_audio_lockstep): on = 1 starts it (budget = callbacks done so far), 0 stops it
extern "C" __declspec(dllexport) void rex_audio_lockstep(int on) {
  {
    std::lock_guard lock(rex::audio::g_audio_mutex);
    rex::audio::g_audio_budget = rex::audio::g_audio_done.load();
    rex::audio::g_audio_lock_base = rex::audio::g_audio_done.load();
    rex::audio::g_audio_lockstep = on != 0;
    rex::audio::g_xma_lockstep = on != 0;
  }
  rex::audio::g_audio_cv.notify_all();
}
// Sampler: the worker's guest context while it runs a guest callback, else nullptr
extern "C" __declspec(dllexport) void* rex_audio_callback_context() { return rex::audio::g_audio_cb_ctx.load(std::memory_order_relaxed); }
// Allows n more audio callbacks and waits until they ran (1) or timeout_ms passed (0)
extern "C" __declspec(dllexport) int rex_audio_advance(uint32_t n, int timeout_ms) {
  std::unique_lock lock(rex::audio::g_audio_mutex);
  rex::audio::g_audio_budget += n;
  rex::audio::g_audio_cv.notify_all();
  return rex::audio::g_audio_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] {
    return rex::audio::g_audio_done >= rex::audio::g_audio_budget;
  }) ? 1 : 0;
}

// Netplay: the guest audio callback reads the time base once per call; in the lockstep the game gives that thread a
// time derived from the callback number instead of the host clock. Returns 1 and the callback number since the
// lockstep start when the calling thread is the audio worker in the lockstep, else 0.
extern "C" __declspec(dllexport) int rex_audio_worker_callback(uint64_t* index) {
  if (!rex::audio::g_audio_lockstep || rex::thread::current_thread_system_id() != rex::audio::g_audio_worker_tid) return 0;
  *index = rex::audio::g_audio_done - rex::audio::g_audio_lock_base;
  return 1;
}

// Netplay: how many audio clients the guest has registered. The guest registers on its own thread,
// so the count read at a frame boundary is the same in every run of a lockstep session: the
// callbacks start at the first boundary after the registration.
extern "C" __declspec(dllexport) uint32_t rex_audio_registrations() {
  return rex::audio::g_audio_registrations.load(std::memory_order_acquire);
}
