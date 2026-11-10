/**
 * The D3D12 presenter's Present call (d3d12_presenter.cpp PaintAndPresentImpl): its sync interval and whether
 * DXGI_PRESENT_ALLOW_TEARING is passed (DXGI_PRESENT_RESTART always is). Pure, so the port's host tests can pin it
 * (wt-ou1 tests/host/present_choice_host_test.cpp).
 */
#pragma once

#include <cstdint>

namespace rex::ui::present_choice {

struct PresentChoice {
  uint32_t sync_interval;  // 0: present at once; 1: wait for the monitor's next refresh
  bool allow_tearing;      // pass DXGI_PRESENT_ALLOW_TEARING
};

// exclusive: the swap chain is in DXGI exclusive fullscreen. present_vsync: the cvar. swap_chain_allows_tearing: the
// swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING.
// - present_vsync: wait for the refresh (interval 1); never the tearing flag, which DXGI allows only with interval 0.
//   A swap chain created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING may present this way: the swap chain flag only makes
//   tearing possible, the present flag asks for it, so the swap chain is not recreated when the cvar changes.
// - otherwise interval 0, and the tearing flag when the swap chain allows it and is not in exclusive fullscreen (the
//   flag is an invalid call there).
inline PresentChoice Choose(bool exclusive, bool present_vsync, bool swap_chain_allows_tearing) {
  if (present_vsync) {
    return {1, false};
  }
  return {0, swap_chain_allows_tearing && !exclusive};
}

}  // namespace rex::ui::present_choice
