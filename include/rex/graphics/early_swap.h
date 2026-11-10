/**
 * present_before_vblank_wait (command_processor.cpp): what the swap packet does with a swap the GPU thread may
 * already have presented at the vblank wait before it. Pure, so the port's host tests can pin it
 * (wt-ou1 tests/host/early_swap_host_test.cpp).
 */
#pragma once

namespace rex::graphics::early_swap {

struct PacketAction {
  bool issue_swap;  // call IssueSwap at the packet
  bool count_swap;  // count the swap in swap_count_ at the packet
};

// presented_early: the early IssueSwap refreshed the guest output with this swap's picture and counted it.
// matches: the packet's frontbuffer, size and fetch constant are the ones presented early.
inline PacketAction AtSwapPacket(bool presented_early, bool matches) {
  if (!presented_early) return {true, true};
  if (matches) return {false, false};
  // Presented early but not this picture (unreachable by reading: the peek and the packet read the same ring bytes
  // below the write-pointer snapshot, so keep the compare exact). Present it again; it was counted already.
  return {true, false};
}

}  // namespace rex::graphics::early_swap
