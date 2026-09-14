#pragma once

// Constants shared by the golden-vector generator (tools/golden_gen, which
// links Nuked-OPN2) and the golden test (test/golden_test.cpp, which does not).

#include <ym2612_eg/ym2612_eg.hpp>

namespace ym2612_eg {
namespace golden {

// The Nuked-OPN2 revision the vectors were generated from.  Kept in lockstep
// with the GIT_TAG in tools/golden_gen/CMakeLists.txt.
inline constexpr char kNukedRepo[] = "https://github.com/nukeykt/Nuked-OPN2";
inline constexpr char kNukedCommit[] =
    "335747d78cb0abbc3b55b004e62dad9763140115";

// Nuked computes eg_out one output sample after the eg_level it is derived
// from (OPN2_EnvelopeGenerate runs at cycle slot+1, OPN2_EnvelopeADSR at
// cycle slot+2), so our output() at sample i is Nuked's eg_out at sample i+1.
inline constexpr int kOutputLagSamples = 1;

// The EG counter value one tick earlier; the counter skips 0 on overflow.
inline int counter_unstep(int v) { return v == 1 ? 0x0FFF : (v - 1) & 0x0FFF; }

// Nuked reads eg_out one pipeline stage ahead of the level it belongs to:
// OPN2_EnvelopeSSGEG runs at cycle `slot`, OPN2_EnvelopeGenerate at slot+1 and
// OPN2_EnvelopeADSR only at slot+2, so eg_out pairs a level with the inversion
// flag that *that same level* produced.  output() instead pairs the new level
// with the inversion decided on the previous one.  The two agree everywhere
// except on the single sample where the inversion actually flips, so the output
// trace is only compared when nothing can flip it -- SSG-EG off, or the
// alternate bit (bit 1 of $90) clear.
inline bool output_comparable(const OperatorParams &op) {
  return !(op.ssg & 0x08) || !(op.ssg & 0x02);
}

} // namespace golden
} // namespace ym2612_eg
