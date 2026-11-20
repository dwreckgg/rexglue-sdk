/**
 * @file        core/seh_posix.cpp
 * @brief       POSIX platform SEH implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <pthread.h>
#include <signal.h>

#include <array>
#include <cstdlib>  // std::abort()

#include <rex/platform/exceptions.h>

namespace rex::platform {

namespace {
// The handlers installed before seh_initialize (the runtime's ExceptionHandler, which serves write-watched guest
// pages): a fault outside an SEH region goes on to them (from upstream 506e6a41, PreviousAction / ForwardSignal).
constexpr std::array kSehSignals = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
std::array<struct sigaction, kSehSignals.size()> g_previous{};

struct sigaction* PreviousAction(int signal_number) {
  for (size_t i = 0; i < kSehSignals.size(); ++i) {
    if (kSehSignals[i] == signal_number) {
      return &g_previous[i];
    }
  }
  return nullptr;
}

// Calls `previous` as the kernel would have, or ends the process with the default action when it has none.
void ForwardSignal(const struct sigaction& previous, int sig, siginfo_t* info, void* ucontext) {
  if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  sigset_t saved;
  pthread_sigmask(SIG_BLOCK, &previous.sa_mask, &saved);
  if (previous.sa_flags & SA_SIGINFO) {
    previous.sa_sigaction(sig, info, ucontext);
  } else {
    previous.sa_handler(sig);
  }
  pthread_sigmask(SIG_SETMASK, &saved, nullptr);
}
}  // namespace

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t /*code*/, void* /*ep*/) {
  // Not used on POSIX - signal handlers throw directly
  return 0;
}

/// Signal handler for SIGSEGV/SIGBUS/SIGFPE/SIGILL
static void signal_handler(int sig, siginfo_t* info, void* ucontext) {
  // Only handle if we're in SEH-protected code; anything else goes to the handler that was there before
  if (!tls_seh_active) {
    if (struct sigaction* previous = PreviousAction(sig)) {
      ForwardSignal(*previous, sig, info, ucontext);
      return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }

  // Determine exception code based on signal
  SehException::Code code;
  switch (sig) {
    case SIGSEGV:
      code = SehException::ACCESS_VIOLATION;
      break;
    case SIGBUS:
      code = SehException::IN_PAGE_ERROR;
      break;
    case SIGFPE:
      code = SehException::FLOAT_DIVIDE_BY_ZERO;
      break;
    case SIGILL:
      code = SehException::ILLEGAL_INSTRUCTION;
      break;
    default:
      code = SehException::UNKNOWN;
      break;
  }

  // Get fault address
  uintptr_t address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

  // Store in thread state for potential rethrow
  tls_seh_state.code = static_cast<uint32_t>(code);
  tls_seh_state.info[0] = 0;
  tls_seh_state.info[1] = address;

  // Use libunwind to throw from signal context
  // This works because libunwind can unwind through signal frames
  throw SehException(code, address);
}

[[noreturn]] void seh_rethrow() {
  // Map stored code back to signal and re-raise
  uint32_t code = tls_seh_state.code;
  int sig;
  switch (code) {
    case SehException::ACCESS_VIOLATION:
      sig = SIGSEGV;
      break;
    case SehException::IN_PAGE_ERROR:
      sig = SIGBUS;
      break;
    case SehException::FLOAT_DIVIDE_BY_ZERO:
    case SehException::INTEGER_DIVIDE_BY_ZERO:
      sig = SIGFPE;
      break;
    case SehException::ILLEGAL_INSTRUCTION:
      sig = SIGILL;
      break;
    default:
      abort();
  }
  // Restore default handler and re-raise
  signal(sig, SIG_DFL);
  raise(sig);
  abort();  // Should not reach here
}

void seh_initialize() {
  if (g_seh_initialized.exchange(true)) {
    return;  // Already initialized
  }

  struct sigaction sa;
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // SA_NODEFER allows re-entry for nested exceptions

  for (size_t i = 0; i < kSehSignals.size(); ++i) {
    sigaction(kSehSignals[i], &sa, &g_previous[i]);
  }
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
