/**
 * One canonical NaN out of every vector dot product (vmsum3fp128 / vmsum4fp128).
 *
 * The code generator turns these two guest instructions into simde_mm_dp_ps(a, b, 0xEF) and
 * simde_mm_dp_ps(a, b, 0xFF) (src/codegen/builders/vector.cpp). When an input is a NaN, DPPS
 * hands back one of the input payloads, and which one depends on the CPU: an AMD and an Intel
 * core give different bits for the same inputs (measured by tests/host/dpps_host_test.cpp in the
 * DOA4 port). A program that hashes its state per frame sees those bits, so two machines that
 * run the same guest code desync on the first dot product that meets a NaN.
 *
 * This header makes every NaN lane of a dot product's result the bit pattern 0xFFC00000 (sign
 * set, quiet bit set, payload zero: the value x86 itself returns for an invalid operation, so
 * inf*0 and inf-inf already give it). Every other lane is returned bit for bit.
 *
 * It is included by <rex/ppc/intrinsics.h>, after simde's sse4.1 header, and so reaches generated
 * code through <rex/ppc.h>. No generated file changes.
 */
#pragma once

#include <simde/x86/sse.h>
#include <simde/x86/sse2.h>
#include <simde/x86/sse4.1.h>

namespace rex::ppc {

// Replaces every NaN lane of `r` (a dot product's result) with 0xFFC00000. An unordered compare of
// the vector with itself is all ones exactly in the NaN lanes, quiet or signalling, whatever the
// payload and sign; the blend takes the canonical NaN there and `r` everywhere else.
inline simde__m128 CanonicalizeDotProductNaN(simde__m128 r) {
  const simde__m128 is_nan = simde_mm_cmpunord_ps(r, r);
  const simde__m128 canonical = simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<int>(0xFFC00000u)));
  return simde_mm_blendv_ps(r, canonical, is_nan);
}

}  // namespace rex::ppc

// The hardware (or simde's portable) dot product itself, with no redirect: what simde_mm_dp_ps was
// before this header redefined it. The two branches mirror simde's own definition
// (thirdparty/simde/simde/x86/sse4.1.h):
//   native SSE4.1 path (SIMDE_X86_SSE4_1_NATIVE, the game's build):
//     #define simde_mm_dp_ps(a, b, imm8) _mm_dp_ps(a, b, imm8)
//   portable path: simde_mm_dp_ps is an inline function.
// Neither branch names the macro defined below, so the redirect cannot recurse. imm8 stays the
// caller's constant expression.
#if defined(SIMDE_X86_SSE4_1_NATIVE)
#define REX_PPC_RAW_DOT_PRODUCT_PS(a, b, imm8) _mm_dp_ps((a), (b), (imm8))
#else
// The parentheses around the name stop a function-like macro of that name from expanding here.
#define REX_PPC_RAW_DOT_PRODUCT_PS(a, b, imm8) (simde_mm_dp_ps)((a), (b), (imm8))
#endif

// The redirect: every simde_mm_dp_ps the code generator emits (23,791 call sites in the DOA4 port)
// now returns the canonicalised result. A function-like macro, so the generated text is unchanged
// and the mask stays a compile-time constant. simde's own macro (native path) is dropped first.
//
// DEVIATION FROM THE NETPLAY SPEC (section 4, "offline play is unchanged"): this is always on, not
// gated by doa4::np::Active(). The only offline change is the payload bits of a NaN that a dot
// product returns; a runtime test of the session state at 23,791 sites would cost more than it
// protects. It is a compile-time property of the build, so a recording made by another build is
// already refused by the recording's build key.
#undef simde_mm_dp_ps
#define simde_mm_dp_ps(a, b, imm8) \
  (::rex::ppc::CanonicalizeDotProductNaN(REX_PPC_RAW_DOT_PRODUCT_PS((a), (b), (imm8))))
