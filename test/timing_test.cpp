// The timing answers -- phase_durations(), ssg_loop_period_ms(),
// sl_skip_probability() -- and the register arithmetic behind them, checked
// against the envelope stepped sample by sample and drawn by sample_curve().

#include "check.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

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

/// A held-forever sample_curve() run: the curve the timing answers are
/// checked against.
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
    // ... and it agrees with output(), which does the inverting. AR is zeroed
    // as for a release: the key-on sample would snap an instant attack to 0.
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
  // ... but SL = 0 leaves the decay on its first sample, with no update,
  // exactly as the chip does, so DR = 0 costs nothing there.
  CHECK(std::isfinite(lifetime_of(adsr(31, 0, 0, 5, 7, 0))));
  // And a phase that never ends means the ones after it never start: it is
  // the phase itself that is infinite, not just the sum.
  CHECK(!std::isfinite(phase_durations(adsr(0, 10, 2, 5, 7, 0), note(60)).attack_ms));
  CHECK(std::isfinite(
      phase_durations(adsr(31, 10, 2, 0, 7, 0), note(60)).sustain_start_ms()));
}

// ------------------------------------------------------------ the loop period

/// ssg_loop_period_ms() against the loop sample_curve() draws, wherever three
/// folds fit in the horizon; the alternating modes count two ramps to a period
/// and the rest one.
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
                // ssg_loop_period_ms() averages kMeasuredLoopPeriods periods;
                // a slow loop reaches the end of kMeasurableMs first and so
                // averages fewer.
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

/// A ramp through a phase that never advances never reaches the fold: no loop,
/// which sample_curve() flags as SsgNeverLoops. SL = 0 leaves DR unused and
/// SL = 15 leaves SR unused, so a zero rate there still loops.
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

/// The alternating modes fold twice per visible period, so their period is two
/// of their own ramps: exactly twice the plain mode's where every ramp is the
/// same length, and wherever the ramps vary, the period sample_curve() draws.
void test_the_alternating_modes_count_two_ramps() {
  // DR = 31 at SL = 15: every ramp is sixteen steps of 32.
  const double plain = period_of(ssg_patch(0, 31, 31, 15, 8, 7, 0));
  const double alternating = period_of(ssg_patch(2, 31, 31, 15, 8, 7, 0));
  CHECK(plain > 0.0);
  CHECK_REL(alternating, plain * 2.0, 1e-9);
  for (const int ar : {31, 20}) {
    for (const int type : {0, 2}) {
      const OperatorParams op = ssg_patch(type, ar, 20, 4, 8, 7, 0);
      const CurveResult held =
          simulate_held(op, note(kReferenceMidiNote), 60000.0);
      CHECK(held.loop_hz > 0.0);
      CHECK_REL(period_of(op), 1000.0 / held.loop_hz, 1e-9);
    }
  }
}

/// At SSG-EG DR rates 57-59, whose 4x step reaches 32, phase_durations() and
/// ssg_loop_period_ms() run the envelope from the key-on phase sample_curve()
/// draws, and agree with the curve.
void test_the_timing_follows_the_sustain_window() {
  const auto first_ms = [](const CurveResult &curve, MarkerKind kind,
                           double after) {
    for (const Marker &m : curve.markers)
      if (m.kind == kind && m.ms > after)
        return static_cast<double>(m.ms);
    return -1.0;
  };
  const auto near_ms = [](double a, double b) {
    return std::fabs(a - b) <= 2e-3 + 1e-6 * std::fabs(b);
  };
  int skipped = 0, hit = 0, loops = 0;
  for (const int midi : {48, 60, 72, 84})
    for (const int ks : {0, 3})
      for (const int dr : {27, 28, 29})
        for (const int sl : {1, 2, 4, 9, 14})
          for (const int ar : {31, 20})
            for (const int type : {0, 1, 2})
              for (const int sr : {0, 12}) {
                const OperatorParams op = ssg_patch(type, ar, dr, sl, sr, 15, ks);
                const NotePitch pitch = note(midi);
                const int rate = detail::effective_rate(dr, key_scale_value(op, pitch));
                if (rate < 57 || rate > 59)
                  continue;
                const CurveResult held = simulate_held(op, pitch, 60000.0);
                const PhaseDurations phases = phase_durations(op, pitch);
                // The first ramp ends at the first fold after the attack.
                const double attack = first_ms(held, MarkerKind::AttackEnd, -1.0);
                const double fold = first_ms(held, MarkerKind::SsgFold, attack);
                double decay = first_ms(held, MarkerKind::DecayEnd, -1.0);
                if (fold >= 0.0 && decay > fold)
                  decay = -1.0;
                CHECK(near_ms(attack, phases.attack_ms));
                if (decay >= 0.0) {
                  ++hit;
                  CHECK(near_ms(decay, phases.sustain_start_ms()));
                  CHECK(phases.sustain_ms > 0.0);
                } else {
                  ++skipped;
                  CHECK(phases.sustain_ms == 0.0);
                }
                if ((type & 1) != 0) {
                  // Mode 1 cuts to silence at the fold.
                  CHECK((marker_ms(held, MarkerKind::Silence) >= 0.0) ==
                        std::isfinite(phases.lifetime_ms()));
                  continue;
                }
                const double period = ssg_loop_period_ms(op, pitch);
                const bool never = std::find(held.warnings.begin(), held.warnings.end(),
                                             CurveWarning::SsgNeverLoops) !=
                                   held.warnings.end();
                CHECK(never == !std::isfinite(period));
                CHECK((held.loop_hz > 0.0) == std::isfinite(period));
                if (held.loop_hz > 0.0) {
                  ++loops;
                  CHECK_REL(period, 1000.0 / held.loop_hz, 1e-9);
                }
              }
  // The sweep reaches both outcomes, and loops among them.
  CHECK(skipped > 20);
  CHECK(hit > 20);
  CHECK(loops > 20);
  std::cout << "\n    first ramp hit " << hit << ", skipped " << skipped
            << ", loops " << loops << "  ... ";
}

/// Keys on `alignment` samples into the EG tick that takes the counter past
/// `phase` and steps to the end of the first decay: 1 when it steps past the
/// sustain window, 0 when it lands in it, -1 when the counter wraps first.
int first_ramp_outcome(const OperatorParams &op, NotePitch pitch, int phase,
                       int alignment) {
  EgSimulator sim(op, pitch);
  sim.reset(static_cast<uint16_t>(phase));
  for (int k = 0; k < alignment; ++k)
    sim.step();
  sim.key_on();
  uint64_t samples = static_cast<uint64_t>(alignment);
  bool decaying = false;
  int outcome = -2;
  while (outcome == -2 && samples < 1000000) {
    sim.step();
    ++samples;
    if (!decaying)
      decaying = sim.phase() == EgPhase::Decay;
    else if (sim.phase() == EgPhase::Sustain)
      outcome = 0;
    else if (sim.attenuation() >= kSsgFoldAttenuation)
      outcome = 1;
  }
  // The first step after reset() is an EG tick, and so is every third one.
  const uint64_t ticks = (samples + 2) / 3;
  return outcome != -2 && static_cast<uint64_t>(phase) + ticks > 0x0FFF
             ? -1
             : outcome;
}

/// Every key-on phase -- 4096 counter values, and the three samples of an EG
/// tick -- stepped to the end of its first decay agrees with
/// first_ramp_skips(), and sl_skip_probability() is the share that skips.
void test_the_skip_probability_counts_every_key_on_phase() {
  struct Case {
    int ar, sl, type;
    NotePitch pitch;
  };
  // DR28 at KS0 is rate 57 at block 2, 58 at block 4 and 59 at block 6.
  const NotePitch r57{644, 2}, r58{644, 4}, r59{644, 6};
  std::vector<Case> cases = {
      {31, 4, 1, r58},  {31, 3, 1, r58}, {20, 1, 1, r58}, {20, 2, 1, r58},
      {22, 1, 1, r58},  {23, 2, 1, r58}, {31, 4, 0, r58}, {22, 2, 2, r58},
      {19, 4, 1, r57},  {21, 9, 1, r57}, {26, 7, 1, r59}, {22, 5, 5, r59},
  };
  for (int sl = 1; sl <= 14; ++sl) {
    cases.push_back({31, sl, 1, r57});
    cases.push_back({31, sl, 1, r59});
  }
  int decided = 0;
  for (const Case &c : cases) {
    const OperatorParams op = ssg_patch(c.type, c.ar, 28, c.sl, 0, 15, 0);
    std::vector<int> outcome(4096 * 3);
    int whole = 4096; // phases before the first ramp through the wrap
    for (int phase = 0; phase < 4096; ++phase)
      for (int a = 0; a < 3; ++a) {
        const int o = first_ramp_outcome(op, c.pitch, phase, a);
        CHECK(o != -2);
        outcome[static_cast<size_t>(phase * 3 + a)] = o;
        // A ramp that runs through the counter's wrap is left out.
        if (o < 0) {
          whole = std::min(whole, phase);
          continue;
        }
        CHECK((o == 1) == detail::first_ramp_skips(op, c.pitch, phase + 1, a));
      }
    // Whole turns of the counter's low four bits, over which p is the share.
    whole -= whole % 16;
    CHECK(whole >= 3584);
    int skips = 0;
    for (int i = 0; i < whole * 3; ++i)
      skips += outcome[static_cast<size_t>(i)];
    const double p = sl_skip_probability(op, c.pitch);
    CHECK(p == static_cast<double>(skips) / (3.0 * whole));
    decided += p > 0.0 && p < 1.0;
  }
  CHECK(decided >= 10);
}

/// The patches measured on hardware: DR28 KS0 at block 4 / F-num 644, SSG-EG
/// $09.
void test_the_measured_patches_skip_as_measured() {
  const NotePitch pitch{644, 4};
  CHECK(sl_skip_probability(ssg_patch(1, 31, 28, 4, 0, 15, 0), pitch) == 0.5);
  CHECK(sl_skip_probability(ssg_patch(1, 31, 28, 3, 0, 15, 0), pitch) == 0.0);
  CHECK(sl_skip_probability(ssg_patch(1, 20, 28, 1, 0, 15, 0), pitch) == 1.0);
  CHECK(sl_skip_probability(ssg_patch(1, 20, 28, 2, 0, 15, 0), pitch) == 0.0);
}

/// sample_curve() keys on at kCurveCounterPhase, or where the key-on decides
/// the sustain window, at the first phase from it whose first decay lands in
/// it; curve_counter_phase(op, pitch, true) is the first that skips.
void test_the_curve_keys_on_at_the_first_phase_that_lands() {
  int decided = 0;
  for (const int midi : {36, 48, 60, 72, 84})
    for (const int ks : {0, 3})
      for (const int ar : {31, 26, 23, 22, 21, 20})
        for (int sl = 1; sl <= 14; ++sl) {
          const OperatorParams op = ssg_patch(1, ar, 28, sl, 0, 15, ks);
          const NotePitch pitch = note(midi);
          const double p = sl_skip_probability(op, pitch);
          const int lands = detail::curve_counter_phase(op, pitch, false);
          const int skips = detail::curve_counter_phase(op, pitch, true);
          if (!(p > 0.0 && p < 1.0)) {
            CHECK(lands == kCurveCounterPhase);
            CHECK(skips == kCurveCounterPhase);
            continue;
          }
          ++decided;
          for (int phase = kCurveCounterPhase; phase <= lands; ++phase)
            CHECK(detail::first_ramp_skips(op, pitch, phase + 1, 0) ==
                  (phase != lands));
          for (int phase = kCurveCounterPhase; phase <= skips; ++phase)
            CHECK(detail::first_ramp_skips(op, pitch, phase + 1, 0) ==
                  (phase == skips));
          CHECK(phase_durations(op, pitch).sustain_ms > 0.0);
        }
  CHECK(decided > 20);
  std::cout << "\n    decided by the key-on " << decided << "  ... ";
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
  RUN_TEST(test_the_timing_follows_the_sustain_window);
  RUN_TEST(test_the_skip_probability_counts_every_key_on_phase);
  RUN_TEST(test_the_measured_patches_skip_as_measured);
  RUN_TEST(test_the_curve_keys_on_at_the_first_phase_that_lands);
  RUN_TEST(test_the_clock_scales_every_duration);
  return testing::summary();
}
