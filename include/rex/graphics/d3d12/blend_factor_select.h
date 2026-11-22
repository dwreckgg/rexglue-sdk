/**
 * The blend factor handed to D3D12 (OMSetBlendFactor) on the host render target path. Issue #23.
 *
 * Xenos blend factors 14 and 15 (constant alpha, one minus constant alpha) map to D3D12_BLEND_BLEND_FACTOR and
 * D3D12_BLEND_INV_BLEND_FACTOR in the pipeline (pipeline_cache.cpp kBlendFactorMap), and D3D12 reads those per
 * channel, so the colour channels get the constant's RGB where the console uses its alpha. A draw that blends with
 * constant alpha, and with no constant colour (12, 13), therefore gets the factor (A, A, A, A). A draw that mixes
 * the two cannot be served by one factor and keeps (R, G, B, A), as before; the caller logs it. Only the colour
 * factors are read: the alpha channel of (R, G, B, A) already holds A, so 14/15 in the alpha factors are right
 * either way, and 12/13 there read the constant's alpha too (kBlendFactorAlphaMap). The blend operation is not read:
 * under MIN or MAX D3D12 ignores the factors, so such a target holding 12/13 can make a draw "mixed" without need,
 * which only costs the old drawing and the log line.
 *
 * Pure and header-only, so tests/host/blend_factor_select_host_test.cpp pins it.
 */
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::d3d12 {

enum class HostBlendFactorChoice : uint8_t {
  // (R, G, B, A) as the registers hold it.
  kRgba,
  // (A, A, A, A): some counted target blends with constant alpha, none with constant colour.
  kAlphaReplicated,
  // Some counted target uses constant colour and some uses constant alpha; drawn with (R, G, B, A).
  kMixed,
};

// Bit i set when colour render target i is bound on the host path (bit 1 + i of the bound bits; bit 0 is depth) and
// its 4-bit write mask (nibble i of normalized_color_mask) is nonzero: the targets whose blend state the pipeline
// takes from RB_BLENDCONTROL (pipeline_cache.cpp, the render target loop of GetCurrentStateDescription).
inline uint32_t HostBlendColorRtMask(uint32_t normalized_color_mask,
                                     uint32_t bound_depth_and_color_render_target_bits) {
  uint32_t color_rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool bound = (bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i))) != 0;
    bool written = ((normalized_color_mask >> (i * 4)) & 0xF) != 0;
    if (bound && written) {
      color_rt_mask |= uint32_t(1) << i;
    }
  }
  return color_rt_mask;
}

inline bool IsHostBlendConstantColorFactor(xenos::BlendFactor factor) {
  return factor == xenos::BlendFactor::kConstantColor || factor == xenos::BlendFactor::kOneMinusConstantColor;
}

inline bool IsHostBlendConstantAlphaFactor(xenos::BlendFactor factor) {
  return factor == xenos::BlendFactor::kConstantAlpha || factor == xenos::BlendFactor::kOneMinusConstantAlpha;
}

// blendcontrol: RB_BLENDCONTROL0-3. color_rt_mask: from HostBlendColorRtMask. replicate_constant_alpha: the
// d3d12_constant_alpha_blend_factor setting (false = (R, G, B, A) for every draw, as 0.3.7 did; a mixed draw is still
// reported as kMixed). constant_rgba: RB_BLEND_RED, GREEN, BLUE, ALPHA. factor_out is always written, by copying bits,
// so a NaN constant reaches D3D12 unchanged.
inline HostBlendFactorChoice SelectHostBlendFactor(const reg::RB_BLENDCONTROL blendcontrol[4],
                                                   uint32_t color_rt_mask, bool replicate_constant_alpha,
                                                   const float constant_rgba[4], float factor_out[4]) {
  bool constant_color = false;
  bool constant_alpha = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(color_rt_mask & (uint32_t(1) << i))) {
      continue;
    }
    const reg::RB_BLENDCONTROL bc = blendcontrol[i];
    constant_color = constant_color || IsHostBlendConstantColorFactor(bc.color_srcblend) ||
                     IsHostBlendConstantColorFactor(bc.color_destblend);
    constant_alpha = constant_alpha || IsHostBlendConstantAlphaFactor(bc.color_srcblend) ||
                     IsHostBlendConstantAlphaFactor(bc.color_destblend);
  }
  if (constant_color && constant_alpha) {
    std::memcpy(factor_out, constant_rgba, sizeof(float) * 4);
    return HostBlendFactorChoice::kMixed;
  }
  if (replicate_constant_alpha && constant_alpha) {
    for (uint32_t c = 0; c < 4; ++c) {
      std::memcpy(&factor_out[c], &constant_rgba[3], sizeof(float));
    }
    return HostBlendFactorChoice::kAlphaReplicated;
  }
  std::memcpy(factor_out, constant_rgba, sizeof(float) * 4);
  return HostBlendFactorChoice::kRgba;
}

}  // namespace rex::graphics::d3d12
