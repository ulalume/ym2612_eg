#pragma once

// Rules shared by the golden-vector generator (tools/golden_gen, which links
// Nuked-OPN2) and the golden test (test/golden_test.cpp, which does not).
//
// Everything here is *documentation with teeth*: the generator stamps the
// values it used into the JSON, and the test recomputes them from the case
// parameters and refuses to run if they disagree.  A vector can therefore never
// smuggle in a hand-fitted alignment.
//
// See golden/DISCREPANCIES.md for the measurements these rules come from.

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

// The free-running 12-bit EG counter, which skips 0 on overflow (EG_SPEC 1).
inline int counter_step(int v) {
  const int n = (v + 1) & 0x0FFF;
  return n == 0 ? 1 : n;
}
inline int counter_unstep(int v) { return v == 1 ? 0x0FFF : (v - 1) & 0x0FFF; }

inline int apply_counter_shift(int phase, int shift) {
  for (int i = 0; i < shift; ++i)
    phase = counter_step(phase);
  for (int i = 0; i > shift; --i)
    phase = counter_unstep(phase);
  return phase;
}

// A rate whose increment row is constant cannot expose a phase error at all.
// That covers rows 0/1 (all zero), 48, 52, 56 and 60-63 (all eight cells
// equal), which is why those rates never constrain the alignment.
inline bool row_is_constant(int rate) {
  for (int i = 1; i < 8; ++i)
    if (detail::kIncTable[rate][i] != detail::kIncTable[rate][0])
      return false;
  return true;
}

// EG_SPEC 4 [DIFF].  For rate >= 48 Nuked does not use the published 64x8
// table: it computes the increment from eg_stephi[rate & 3][eg_timer_low_lock],
// and eg_timer_low_lock is a pipeline-latched copy of the timer.  Measured
// against full Nuked traces for every rate 44..63 (golden/DISCREPANCIES.md),
// the result is exactly a one-EG-tick rotation of the table row, in a direction
// that depends only on rate % 4:
//
//   row constant   -> no constraint at all
//   rate < 48      -> agrees with the table exactly, so the shift must be 0
//   rate % 4 == 3  -> Nuked is one tick late  -> our counter must run +1 ahead
//   otherwise      -> Nuked is one tick early -> our counter must run -1 behind
//
// The library deliberately keeps the table (EG_SPEC 4 recommends it, and the
// per-4-tick average is identical); the shift is how the golden test states the
// divergence exactly instead of loosening the comparison.
inline constexpr int kNoConstraint = 99;

inline int counter_shift_for_rate(int rate) {
  if (row_is_constant(rate))
    return kNoConstraint;
  if (rate < 48)
    return 0;
  return (rate & 3) == 3 ? +1 : -1;
}

// Combine the constraints of the four phase rates of one case.  Returns
// kNoConstraint if nothing constrains the alignment, and sets *mixed when two
// phases demand different shifts -- such a case cannot be compared exactly and
// the generator refuses to emit it.
inline int counter_shift_for_case(const int rate[4], bool *mixed) {
  int shift = kNoConstraint;
  *mixed = false;
  for (int i = 0; i < 4; ++i) {
    const int s = counter_shift_for_rate(rate[i]);
    if (s == kNoConstraint)
      continue;
    if (shift == kNoConstraint)
      shift = s;
    else if (shift != s)
      *mixed = true;
  }
  return shift;
}

inline int counter_shift_for_case(const EgSimulator &eg, bool *mixed) {
  const int rate[4] = {
      eg.rate_of(EgPhase::Attack), eg.rate_of(EgPhase::Decay),
      eg.rate_of(EgPhase::Sustain), eg.rate_of(EgPhase::Release)};
  return counter_shift_for_case(rate, mixed);
}

// Nuked runs its envelope state machine once per output sample and advances at
// most one state per sample, while this library evaluates both transitions at
// the top of an EG tick (EG_SPEC "reference tick loop", ymfm's model).  The two
// agree whenever the attack ends *on* an EG tick, because the two spare samples
// before the next tick absorb the Attack->Decay->Sustain pair.  They do not
// agree when the attack ends between ticks, which only happens with the
// instant attack (rate >= 62 forces att = 0 at key-on): SL = 0 then needs both
// transitions and Nuked loses one increment, leaving it exactly one EG tick
// behind us for the rest of the note.  Such cases are excluded from the
// vectors; see golden/DISCREPANCIES.md.
inline bool has_sl0_instant_attack_divergence(const EgSimulator &eg) {
  return eg.rate_of(EgPhase::Attack) >= 62 && eg.sustain_attenuation() == 0;
}

// EG_SPEC 6 [DIFF] / SSG_EG_SPEC 5.1.  Nuked tests Decay -> Sustain with an
// equality on the top six bits ((level >> 4) == (sl5 << 1)), which is a 16-wide
// window; this library uses att >= sustain_att, as ymfm does and as both specs
// recommend.  A plain decay step is at most 8 so the window can never be
// jumped, but SSG-EG quadruples the step and 4 * 4 already clears it.  So the
// two only part company with SSG-EG on, a decay row that contains an increment
// of 4 or more (rate >= 53), and a sustain level the ramp can actually reach
// (SL 1..14; SL 0 matches at once and SL 15 sits above the 0x200 freeze).
inline bool has_ssg_sustain_window_divergence(const OperatorParams &op,
                                              const EgSimulator &eg) {
  if (!(op.ssg & 0x08))
    return false;
  const int sustain = eg.sustain_attenuation();
  if (sustain == 0 || sustain >= kSsgFoldAttenuation)
    return false;
  const int rate = eg.rate_of(EgPhase::Decay);
  for (int i = 0; i < 8; ++i)
    if (4 * detail::kIncTable[rate][i] > 15)
      return true;
  return false;
}

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

// The one-EG-tick counter shift models Nuked's latched timer exactly, but only
// inside a single sweep of the 12-bit counter: the skip-0 wrap (0xFFF -> 1)
// is a discontinuity in `counter & 3`, and a shifted counter crosses it one
// tick away from the real one.  A shifted case must therefore stay inside one
// counter period.
inline bool counter_wraps(int phase, int samples) {
  const int ticks = (samples + 2) / 3;
  return phase + ticks >= 0x0FFF;
}

} // namespace golden
} // namespace ym2612_eg
