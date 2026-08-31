// phase_durations() and ssg_loop_period_ms(): the closed forms, cross-checked
// against the simulator they exist to save.
//
// These two sweeps are the reason the closed forms are allowed to exist at
// all. Everything else here is arithmetic that can be read off a register;
// the sweeps are the claim that the arithmetic is the same answer the chip
// arrives at by running.

#include "check.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace ym2612_eg;

namespace {

constexpr int kReferenceMidiNote = 60; // middle C

OperatorParams adsr(int ar, int dr, int sl, int sr, int rr, int ks) {
  OperatorParams op;
  op.ar = static_cast<uint8_t>(ar);
  op.dr = static_cast<uint8_t>(dr);
  op.sl = static_cast<uint8_t>(sl);
  op.sr = static_cast<uint8_t>(sr);
  op.rr = static_cast<uint8_t>(rr);
  op.ks = static_cast<uint8_t>(ks);
  return op;
}

/// SSG-EG on, `type` being the 3 shape bits: attack, alternate, hold.
OperatorParams ssg_patch(int type, int ar, int dr, int sl, int sr, int rr,
                         int ks) {
  OperatorParams op = adsr(ar, dr, sl, sr, rr, ks);
  op.ssg = static_cast<uint8_t>(0x08 | (type & 0x07));
  return op;
}

NotePitch note(int midi) { return NotePitch::from_midi(midi); }

double lifetime_of(const OperatorParams &op, int midi = kReferenceMidiNote) {
  return phase_durations(op, note(midi)).lifetime_ms();
}

double period_of(const OperatorParams &op, int midi = kReferenceMidiNote) {
  return ssg_loop_period_ms(op, note(midi));
}

/// A held-forever run: the thing the closed forms are checked against. It is
/// the one job a simulation is better at than a formula -- where the envelope
/// really parks, how fast the loop really runs.
CurveResult simulate_held(const OperatorParams &op, NotePitch pitch,
                          double max_ms) {
  CurveRequest request;
  request.op = op;
  request.pitch = pitch;
  request.gate_ms = -1.0;
  request.max_ms = max_ms;
  return sample_curve(request);
}

// ------------------------------------------------------ register arithmetic

void test_sustain_attenuation_matches_the_simulator() {
  // SL = 15 is the special one: 0x3E0, not 15 * 32.
  CHECK_EQ(sustain_attenuation(0), 0);
  CHECK_EQ(sustain_attenuation(2), 64);
  CHECK_EQ(sustain_attenuation(14), 448);
  CHECK_EQ(sustain_attenuation(15), 0x3E0);
  for (int sl = 0; sl <= 15; ++sl) {
    EgSimulator sim(adsr(31, 10, sl, 5, 7, 0), note(kReferenceMidiNote));
    CHECK_EQ(sim.sustain_attenuation(), sustain_attenuation(sl));
  }
}

/// The free function and the simulator's accessor are one value, not two
/// spellings of it: recompute() calls this.
void test_key_scale_value_matches_the_simulator() {
  for (int ks = 0; ks <= 3; ++ks) {
    for (int midi = 12; midi <= 107; ++midi) {
      const OperatorParams op = adsr(31, 10, 2, 5, 7, ks);
      EgSimulator sim(op, note(midi));
      CHECK_EQ(key_scale_value(op, note(midi)), sim.key_scale_value());
    }
  }
  // KS = 0 folds the whole keyboard onto four values, which is why a caller
  // can cache curves by ksv rather than by note.
  const OperatorParams flat = adsr(31, 10, 2, 5, 7, 0);
  CHECK_EQ(key_scale_value(flat, note(60)), key_scale_value(flat, note(72)));
  CHECK(key_scale_value(flat, note(48)) != key_scale_value(flat, note(60)));
}

/// Where a release has to start from. The inverted SSG-EG modes read the
/// scale the other way round, so 0 is their quietest point rather than their
/// loudest.
void test_the_loudest_attenuation_follows_the_inversion() {
  CHECK_EQ(loudest_attenuation(adsr(31, 10, 2, 5, 7, 0)), 0);
  for (int type = 0; type <= 7; ++type) {
    const OperatorParams op = ssg_patch(type, 31, 10, 2, 5, 7, 0);
    const uint16_t want =
        (type & 0x04) != 0 ? kSsgFoldAttenuation : uint16_t{0};
    CHECK_EQ(loudest_attenuation(op), want);
    // ... and it agrees with output(), which is what does the inverting. AR
    // is zeroed exactly as a caller staging a release does it: key_on() snaps
    // an instant attack straight to att = 0 and would throw the level away.
    OperatorParams released = op;
    released.ar = 0;
    EgSimulator sim(released, note(kReferenceMidiNote));
    sim.reset(0, want);
    sim.key_on();
    sim.step(); // the key state reaches the envelope one sample later
    CHECK_EQ(sim.output(), 0); // full volume, whichever end of the scale
  }
  // The shape bits are inert while the enable bit is down.
  OperatorParams disabled = ssg_patch(4, 31, 10, 2, 5, 7, 0);
  disabled.ssg &= 0x07;
  CHECK_EQ(loudest_attenuation(disabled), 0);
}

// -------------------------------------------------- the closed-form lifetime

/// The first marker of `kind`, or -1.
double marker_ms(const CurveResult &curve, MarkerKind kind) {
  for (const Marker &m : curve.markers) {
    if (m.kind == kind) {
      return m.ms;
    }
  }
  return -1.0;
}

/**
 * The claim phase_durations() rests on: the closed form is the number the
 * simulator would have reached, and it reaches it without a horizon.
 *
 * A sweep rather than a handful of cases, because the failure this guards
 * against is not one patch being wrong -- it is a corner of the register
 * space where dividing stops modelling what the chip does by walking.
 *
 * Two comparisons, because the envelope has two landmarks a run can name:
 * where the decay ends, which is sustain_start_ms(), and where the whole
 * thing comes to rest, which is lifetime_ms(). Patches whose landmark is a
 * few EG ticks away are left out -- there the quantisation of a single tick
 * is a large fraction of the answer, and a relative tolerance would be
 * measuring the clock rather than the model.
 *
 * SL = 15 is in the decay half of the sweep and out of the lifetime half.
 * Its sustain level is 0x3E0 and the envelope dies at 0x3F0, so the sustain
 * phase is SIXTEEN attenuation units wide: the decay does not stop at the
 * sustain level but at the first increment past it, and that one overshoot
 * can be half of what is left. The tail is quantisation, not model -- see
 * test_a_full_sustain_level_leaves_a_sixteen_unit_tail().
 */
void test_the_lifetime_agrees_with_the_simulator() {
  constexpr double kFloorMs = 20.0;     // below this, one tick is the error
  constexpr double kCeilingMs = 4000.0; // above this, the sweep costs minutes
  double worst_decay = 0.0;
  double worst_life = 0.0;
  int decays = 0;
  int lives = 0;
  for (const int ar : {31, 20, 14, 8, 1}) {
    for (const int dr : {31, 18, 10, 3}) {
      for (const int sl : {0, 2, 7, 14, 15}) {
        for (const int sr : {31, 20, 8, 4}) {
          for (const int ks : {0, 3}) {
            for (const int midi : {48, 60, 84}) {
              const OperatorParams op = adsr(ar, dr, sl, sr, 7, ks);
              const PhaseDurations phases = phase_durations(op, note(midi));
              CHECK(phases.lifetime_ms() > 0.0);

              // Where the decay ends. Held only as far as it needs to be.
              const double start = phases.sustain_start_ms();
              if (std::isfinite(start) && start >= kFloorMs &&
                  start <= kCeilingMs) {
                const CurveResult run =
                    simulate_held(op, note(midi), start * 1.5 + 100.0);
                const double decay_end = marker_ms(run, MarkerKind::DecayEnd);
                CHECK(decay_end > 0.0);
                const double error = std::fabs(start - decay_end) / decay_end;
                worst_decay = std::max(worst_decay, error);
                ++decays;
                CHECK(error < 0.05);
              }

              // ... and where the whole envelope comes to rest.
              const double lifetime = phases.lifetime_ms();
              if (sl == 15 || !std::isfinite(lifetime) ||
                  lifetime < kFloorMs || lifetime > kCeilingMs) {
                continue;
              }
              const CurveResult run =
                  simulate_held(op, note(midi), lifetime * 1.5 + 100.0);
              CHECK(std::isfinite(run.park_ms));
              const double error =
                  std::fabs(lifetime - run.park_ms) / run.park_ms;
              worst_life = std::max(worst_life, error);
              ++lives;
              CHECK(error < 0.05);
            }
          }
        }
      }
    }
  }
  CHECK(decays > 500);
  CHECK(lives > 500);
  std::cout << "\n    decay end: " << decays << " patches, worst "
            << worst_decay * 100.0 << "%; lifetime: " << lives
            << " patches, worst " << worst_life * 100.0 << "%  ... ";
}

/**
 * SL = 15 is the one place the division has nothing left to average over.
 *
 * The sustain level is 0x3E0 and the envelope dies at 0x3F0, so the sustain
 * is sixteen attenuation units -- a handful of increments, and sometimes
 * fewer, because the decay stops at the first increment PAST the sustain
 * level rather than on it. The shape is still right where it is structural:
 * the decay ends where the chip ends it, which the sweep above checks at
 * SL = 15 like everywhere else. What the last sixteen units then take is
 * quantisation, and no closed form that divides can report it to a few
 * percent -- with a slow SR it can be out by half.
 */
void test_a_full_sustain_level_leaves_a_sixteen_unit_tail() {
  CHECK_EQ(sustain_attenuation(15), 0x3E0);
  const OperatorParams op = adsr(31, 18, 15, 4, 7, 0);
  const PhaseDurations phases = phase_durations(op, note(kReferenceMidiNote));
  // The sustain is what is left between 0x3E0 and the 0x3F0 cut, and nothing
  // more: at the same rate a SL = 14 patch would cross 0x3F0 - 0x1C0 units.
  const PhaseDurations wider =
      phase_durations(adsr(31, 18, 14, 4, 7, 0), note(kReferenceMidiNote));
  CHECK_REL(phases.sustain_ms, wider.sustain_ms * 16.0 / (0x3F0 - 0x1C0),
            1e-9);
  // ... and the decay has the rest: 0x3E0 units where SL = 14 gets 0x1C0.
  CHECK_REL(phases.decay_ms, wider.decay_ms * 0x3E0 / 0x1C0, 1e-9);
}

/// SSG-EG quadruples every post-attack increment and stops the ramp at 0x200
/// instead of 0x3F0, so the same registers live a small fraction as long.
void test_ssg_eg_shortens_the_lifetime() {
  const OperatorParams plain = adsr(31, 20, 4, 10, 7, 0);
  // Hold: not a loop, so the ramp really is the whole life.
  const OperatorParams held = ssg_patch(1, 31, 20, 4, 10, 7, 0);
  CHECK(lifetime_of(held) > 0.0);
  CHECK(lifetime_of(held) < lifetime_of(plain) * 0.2);
}

/**
 * A rate of 0 never advances its phase, so the envelope never gets past it.
 * That is what SR = 0 holding forever *is*, and reporting it as infinite --
 * rather than as "the run saw no decay" -- is the whole reason a caller can
 * tell SR = 0 from SR = 31 without simulating either.
 */
void test_a_rate_of_zero_lasts_forever() {
  CHECK(!std::isfinite(lifetime_of(adsr(31, 10, 2, 0, 7, 0))));  // SR = 0
  CHECK(!std::isfinite(lifetime_of(adsr(31, 0, 2, 5, 7, 0))));   // DR = 0, SL > 0
  CHECK(!std::isfinite(lifetime_of(adsr(0, 10, 2, 5, 7, 0))));   // AR = 0
  // ... but SL = 0 skips the decay outright, exactly as the chip does, so
  // DR = 0 costs nothing there.
  CHECK(std::isfinite(lifetime_of(adsr(31, 0, 0, 5, 7, 0))));
  // And a phase that never ends means the ones after it never start: it is
  // the phase itself that is infinite, not just the sum.
  CHECK(!std::isfinite(phase_durations(adsr(0, 10, 2, 5, 7, 0), note(60)).attack_ms));
  CHECK(std::isfinite(
      phase_durations(adsr(31, 10, 2, 0, 7, 0), note(60)).sustain_start_ms()));
}

// ----------------------------------------------- the closed-form loop period

/**
 * The counterpart of the lifetime sweep, for ssg_loop_period_ms(): the period
 * computed from the registers is the period the chip actually runs at.
 *
 * Checked only where a simulation can answer at all -- sample_curve() needs
 * three folds before it will publish loop_hz, so a loop slower than a third
 * of the window here has no measurement to be compared against. That is
 * precisely the blindness the closed form exists to cure.
 *
 * Both fold conventions are covered: the alternating modes (types 2, 3, 6, 7)
 * count two ramps to a period and the rest one, and getting that wrong is a
 * clean factor of two rather than a few percent.
 */
void test_the_loop_period_agrees_with_the_simulator() {
  constexpr double kMeasurableMs = 12000.0;
  double worst = 0.0;
  double worst_instant_attack = 0.0;
  int compared = 0;
  for (const int type : {0, 2, 4, 6}) {
    // AR = 14 is the weak corner of the model and belongs in the grid: below
    // 31 the fold is followed by a real attack, and that attack's length is
    // decided by where the climb before it left the shared counter.
    for (const int ar : {31, 20, 14}) {
      for (const int dr : {31, 24, 18, 12, 8}) {
        for (const int sl : {0, 9, 14, 15}) {
          for (const int sr : {31, 8, 3}) {
            for (const int ks : {0, 3}) {
              for (const int midi : {48, 72, 84}) {
                const OperatorParams op = ssg_patch(type, ar, dr, sl, sr, 0, ks);
                const double analytic = ssg_loop_period_ms(op, note(midi));
                CHECK(analytic > 0.0); // every one of these is a looping mode
                // Three folds, and the alternating modes need two per period.
                if (!std::isfinite(analytic) || analytic * 4.0 > kMeasurableMs) {
                  continue;
                }
                const CurveResult held =
                    simulate_held(op, note(midi), kMeasurableMs);
                CHECK(held.loop_hz > 0.0);
                const double simulated = 1000.0 / held.loop_hz;
                const double error =
                    std::fabs(analytic - simulated) / simulated;
                worst = std::max(worst, error);
                if (ar == 31) {
                  worst_instant_attack = std::max(worst_instant_attack, error);
                }
                ++compared;
                // A few percent. What is left is not a modelling error but
                // the loop's own shape: a ramp ends on a slot boundary of
                // whichever rate carried it there, so successive ramps can
                // differ by a slot, and the two answers average a different
                // number of them.
                CHECK(error < 0.04);
              }
            }
          }
        }
      }
    }
  }
  CHECK(compared > 500);
  std::cout << "\n    loop period: " << compared
            << " patches cross-checked, worst disagreement " << worst * 100.0
            << "%, " << worst_instant_attack * 100.0 << "% at AR = 31  ... ";
}

/**
 * A ramp with a phase that never advances never reaches the fold, so the
 * envelope never loops again -- which is not a slow loop but no loop.
 *
 * These are exactly the patches sample_curve() flags as SsgNeverLoops, and
 * the closed form arrives at the same three by arithmetic rather than by a
 * rule: an infinite phase makes the ramp infinite, and only the phases the
 * ramp actually needs are in the sum. SL = 0 skips the decay outright, so
 * DR = 0 costs nothing there; SL = 15 puts the sustain level above the fold,
 * so SR never runs and SR = 0 costs nothing.
 */
void test_a_ramp_that_never_finishes_is_no_loop_at_all() {
  CHECK(!std::isfinite(period_of(ssg_patch(0, 31, 0, 8, 8, 7, 0))));  // DR = 0
  CHECK(!std::isfinite(period_of(ssg_patch(0, 31, 15, 8, 0, 7, 0)))); // SR = 0
  CHECK(!std::isfinite(period_of(ssg_patch(0, 0, 15, 8, 8, 7, 0))));  // AR = 0
  CHECK(std::isfinite(period_of(ssg_patch(0, 31, 0, 0, 8, 7, 0))));   // SL = 0
  CHECK(std::isfinite(period_of(ssg_patch(0, 31, 15, 15, 0, 7, 0)))); // SL = 15
  // A mode that latches instead of folding is not a loop either, and says so
  // with a plain zero rather than an infinity: nothing is stalled, the shape
  // simply has no period.
  for (const int hold_type : {1, 3, 5, 7}) {
    CHECK(period_of(ssg_patch(hold_type, 31, 15, 8, 8, 7, 0)) == 0.0);
  }
  CHECK(period_of(adsr(31, 15, 8, 8, 7, 0)) == 0.0); // SSG-EG off
  // ... and the ones that do stall are the ones sample_curve() names.
  const CurveResult stalled = simulate_held(ssg_patch(0, 31, 15, 8, 0, 7, 0),
                                            note(kReferenceMidiNote), 500.0);
  CHECK(std::find(stalled.warnings.begin(), stalled.warnings.end(),
                  CurveWarning::SsgNeverLoops) != stalled.warnings.end());
}

/// The alternating modes fold twice per visible period, so they are exactly
/// twice their own ramp -- a factor of two, not a few percent.
void test_the_alternating_modes_count_two_ramps() {
  for (const int ar : {31, 20}) {
    const double plain = period_of(ssg_patch(0, ar, 20, 4, 8, 7, 0));
    const double alternating = period_of(ssg_patch(2, ar, 20, 4, 8, 7, 0));
    CHECK(plain > 0.0);
    CHECK_REL(alternating, plain * 2.0, 1e-9);
  }
}

/// The clock is an argument, not a constant: PAL runs about 1% slower and
/// every duration follows it.
void test_the_clock_scales_every_duration() {
  const OperatorParams op = adsr(20, 12, 4, 8, 7, 0);
  const PhaseDurations ntsc = phase_durations(op, note(60), kNtscClockHz);
  const PhaseDurations pal = phase_durations(op, note(60), kPalClockHz);
  CHECK_REL(pal.lifetime_ms(), ntsc.lifetime_ms() * kNtscClockHz / kPalClockHz,
            1e-9);
  const OperatorParams loop = ssg_patch(0, 31, 20, 4, 8, 7, 0);
  CHECK_REL(ssg_loop_period_ms(loop, note(60), kPalClockHz),
            ssg_loop_period_ms(loop, note(60), kNtscClockHz) * kNtscClockHz /
                kPalClockHz,
            1e-9);
}

} // namespace

int main() {
  std::cout << "timing_test\n";
  RUN_TEST(test_sustain_attenuation_matches_the_simulator);
  RUN_TEST(test_key_scale_value_matches_the_simulator);
  RUN_TEST(test_the_loudest_attenuation_follows_the_inversion);
  RUN_TEST(test_the_lifetime_agrees_with_the_simulator);
  RUN_TEST(test_a_full_sustain_level_leaves_a_sixteen_unit_tail);
  RUN_TEST(test_ssg_eg_shortens_the_lifetime);
  RUN_TEST(test_a_rate_of_zero_lasts_forever);
  RUN_TEST(test_the_loop_period_agrees_with_the_simulator);
  RUN_TEST(test_a_ramp_that_never_finishes_is_no_loop_at_all);
  RUN_TEST(test_the_alternating_modes_count_two_ramps);
  RUN_TEST(test_the_clock_scales_every_duration);
  return testing::summary();
}
