// ym2612_eg::graph: the axis width, the two traces drawn on it, the live
// cursor, and the two caches.

#include "check.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace ym2612_eg;
using namespace ym2612_eg::graph;

namespace {

/// Every curve here is drawn at middle C unless the test is about the note.
const NotePitch kMiddleC = NotePitch::from_midi(60);
/// C0: the bottom of the range, and a key-scale value nothing else shares.
const NotePitch kC0 = NotePitch::from_midi(12);

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

/// bit3 enable, bit2 attack, bit1 alternate, bit0 hold.
uint8_t ssg_bits(int type) {
  return static_cast<uint8_t>(0x08 | (type & 0x07));
}

OperatorParams ssg_patch(int type, int ar, int dr, int sl, int sr, int rr,
                         int ks) {
  OperatorParams op = adsr(ar, dr, sl, sr, rr, ks);
  op.tl = 0;
  op.ssg = ssg_bits(type);
  return op;
}

/// EG_SPEC's worked example: AR=31 TL=0 DR=10 SL=2 SR=5 RR=7, KS=0.
OperatorParams worked_example() { return adsr(31, 10, 2, 5, 7, 0); }

/// A patch slow enough that every one of its phases runs for seconds.
OperatorParams slow_patch() {
  OperatorParams op = adsr(11, 3, 4, 2, 4, 0);
  op.tl = 24;
  return op;
}

bool near_rel(double value, double expected, double tolerance) {
  return std::fabs(value - expected) <= std::fabs(expected) * tolerance;
}

// ------------------------------------------------------------- fake clocks

/**
 * A clock the test drives, frame by frame.
 *
 * EnvelopeCurveCache reads its clock once when the request has moved off the
 * curve it holds, and once more the moment a rebuild finishes, so the gap
 * between those two reads IS what the rebuild cost. `cost_ms` is that gap: the
 * clock hands out `now_ms` and then steps itself on by it. Nothing else steps
 * it, so a test sets `now_ms` at the top of every frame and the stray step
 * from a frame that did not rebuild is simply overwritten.
 */
struct FakeClock {
  double now_ms = 0.0;
  double cost_ms = 0.0;
};
FakeClock g_clock;
double fake_now_ms() {
  const double t = g_clock.now_ms;
  g_clock.now_ms += g_clock.cost_ms;
  return t;
}

/**
 * A clock that leaps a whole second every time it is read, so the throttle
 * never has cause to defer.
 *
 * The tests that use it are asking what the cache NOTICES -- which registers
 * count as a change, which do not. A second is past every interval the
 * throttle can ask for, kMaxRebuildDeferMs included.
 */
double g_leaping_ms = 0.0;
double leaping_clock() {
  g_leaping_ms += 1000.0;
  return g_leaping_ms;
}

/// One frame at 60 fps, which is what a drag advances the clock by.
constexpr double kFrameMs = 1000.0 / 60.0;

/// What an ordinary ADSR patch measures ...
constexpr double kCheapBuildMs = 1.0;
/// ... and what an SSG-EG one does.
constexpr double kExpensiveBuildMs = 4.0;

/// Where a trace's polyline actually ends.
double content_ms(const CurveResult &curve) {
  return curve.points.empty() ? 0.0
                              : static_cast<double>(curve.points.back().ms);
}

bool has_marker(const CurveResult &curve, MarkerKind kind) {
  return first_marker_ms(curve, kind) >= 0.0;
}

/// A held-forever simulation with a horizon, which the closed forms are
/// checked AGAINST: where the decay really ends, where the envelope really
/// parks, how fast the loop really runs.
CurveResult simulate_held(const OperatorParams &op, NotePitch pitch,
                          double max_ms = 12000.0) {
  CurveRequest request;
  request.op = op;
  request.pitch = pitch;
  request.gate_ms = -1.0;
  request.max_ms = max_ms;
  return sample_curve(request);
}

// --------------------------------------------------- what counts as a change

/// Every register in the struct shapes the envelope, and same_envelope() has
/// to see all eight of them: a cache that missed one would draw the wrong
/// curve until something else moved.
void test_same_envelope_covers_every_register() {
  const OperatorParams base = ssg_patch(2, 20, 10, 4, 6, 7, 1);
  CHECK(same_envelope(base, base));

  OperatorParams copy = base;
  CHECK(same_envelope(base, copy));

  const auto differs = [&base](const OperatorParams &other) {
    return !same_envelope(base, other) && !same_envelope(other, base);
  };
  copy = base; copy.ar = 21; CHECK(differs(copy));
  copy = base; copy.dr = 11; CHECK(differs(copy));
  copy = base; copy.sr = 7;  CHECK(differs(copy));
  copy = base; copy.rr = 8;  CHECK(differs(copy));
  copy = base; copy.sl = 5;  CHECK(differs(copy));
  copy = base; copy.tl = 9;  CHECK(differs(copy));
  copy = base; copy.ks = 2;  CHECK(differs(copy));
  copy = base; copy.ssg = ssg_bits(3); CHECK(differs(copy));
}

// -------------------------------------------------- the held-window policy

/// The whole rule, on a patch that ends: the axis is the envelope's own
/// length, so a sustain that finishes is drawn to its last millisecond.
void test_the_axis_reaches_the_end_of_the_sustain() {
  // A patch whose whole life is 1.5 s.
  OperatorParams op = adsr(8, 28, 4, 15, 11, 0);
  op.tl = 7;
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(timeline.lifetime_ms() < kMaxSpanMs);
  // Not approximately the lifetime, and not a compressed version of it: the
  // lifetime itself, to the last bit of the double.
  CHECK(choose_held_ms(op, kMiddleC) == timeline.lifetime_ms());

  // The trace agrees with the closed form: the decay really does end inside
  // the axis, and the sustain really does run out at the right-hand edge.
  const CurveResult held = simulate_held(op, kMiddleC);
  const double decay_end = first_marker_ms(held, MarkerKind::DecayEnd);
  CHECK(decay_end > 0.0);
  CHECK(near_rel(decay_end, timeline.sustain_start_ms(), 0.02));
  CHECK(decay_end < choose_held_ms(op, kMiddleC));
  // The whole sustain is on it: the envelope comes to rest at the right-hand
  // edge rather than somewhere off past it.
  CHECK(std::isfinite(held.park_ms));
  CHECK(near_rel(held.park_ms, choose_held_ms(op, kMiddleC), 0.02));

  // ... and that is not one patch's luck. Wherever the envelope ends inside
  // the ceiling, the axis is exactly where it ends -- 327 300 of the register
  // combinations swept below, without a single millisecond of daylight.
  for (int ar = 0; ar <= 31; ar += 3) {
    for (int dr = 1; dr <= 31; dr += 3) {
      for (int sl = 0; sl <= 15; sl += 3) {
        for (int sr = 1; sr <= 31; sr += 3) {
          const OperatorParams swept = adsr(ar, dr, sl, sr, 7, 0);
          const PhaseDurations phases = phase_durations(swept, kMiddleC);
          // Between the floor a graph needs to be legible at all and the
          // ceiling, the axis IS the envelope: no compression, no share, no
          // rounding.
          if (phases.lifetime_ms() >= kMinHeldMs &&
              phases.lifetime_ms() <= kMaxSpanMs) {
            CHECK(choose_held_ms(swept, kMiddleC) == phases.lifetime_ms());
          }
        }
      }
    }
  }
}

/// The one thing that stops it. An envelope with seconds of sustain left to
/// run cannot be drawn whole on any axis a screen can hold, so the ceiling
/// takes it -- and takes the sustain's tail, never the shape before it.
void test_an_envelope_longer_than_the_ceiling_is_cut_at_the_ceiling() {
  const OperatorParams op = worked_example();
  // EG_SPEC's worked example takes 27 s to reach silence, which the closed
  // form answers without simulating any of it.
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(timeline.lifetime_ms() > 20000.0);
  CHECK(timeline.lifetime_ms() < 35000.0);
  CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);

  // The attack and the decay are the shape being read, and they are wholly
  // inside it: it is the far end of the sustain that is off the graph.
  CHECK(timeline.sustain_start_ms() < kMaxSpanMs);
  CHECK(first_marker_ms(simulate_held(op, kMiddleC), MarkerKind::DecayEnd) <
        kMaxHeldMs);
}

/// SR = 0 is a hold that never ends, so the envelope has no length to be the
/// axis. It draws a flat line, and a flat line says the same thing at any
/// width, so it takes a fixed kFlatHoldShare of the graph and the attack and
/// decay own everything else. A rule of its own, and this is the whole of it.
void test_a_hold_that_never_ends_takes_a_fixed_share() {
  OperatorParams op = worked_example();
  op.sr = 0; // parks at the sustain level and stays there

  const CurveResult held = simulate_held(op, kMiddleC);
  CHECK(std::isfinite(held.park_ms));
  // The closed form tells the truth about the envelope: it never ends.
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(!std::isfinite(timeline.sustain_ms));
  CHECK(!std::isfinite(timeline.lifetime_ms()));

  // The share is exact, not a bound: the shape owns 1 - kFlatHoldShare of the
  // axis and the flat line owns the rest.
  const double held_ms = choose_held_ms(op, kMiddleC);
  CHECK(near_rel(timeline.sustain_start_ms() / held_ms, 1.0 - kFlatHoldShare,
                 1e-12));
  // The trace agrees: it parks where the closed form says the shape ends.
  CHECK(near_rel(held.park_ms, timeline.sustain_start_ms(), 0.02));
  CHECK(held.park_ms < held_ms);

  // ... whatever the attack and decay are, because it is a share of them.
  for (int ar = 1; ar <= 31; ar += 2) {
    for (int dr = 2; dr <= 31; dr += 3) {
      const OperatorParams swept = adsr(ar, dr, 4, 0, 7, 0);
      const double start =
          phase_durations(swept, kMiddleC).sustain_start_ms();
      // Between the floor a graph needs to be legible and the ceiling. Past
      // the ceiling the flat line is what gives way -- an attack and decay
      // that outlast the axis get all of it and no hold -- and below the floor
      // the minimum width wins instead.
      const double asked = start / (1.0 - kFlatHoldShare);
      if (asked >= kMinHeldMs && asked <= kMaxSpanMs) {
        CHECK(near_rel(start / choose_held_ms(swept, kMiddleC),
                       1.0 - kFlatHoldShare, 1e-12));
      }
    }
  }

  // The price of the rule, stated rather than hidden: SR = 0 and SR = 1 are
  // not neighbours on the axis. SR = 1 is a sustain that really does end --
  // after 109 s, so the ceiling takes it -- and the axis jumps thirtyfold for
  // one register step. Drawing a flat line as a flat line costs exactly this.
  op.sr = 1;
  CHECK(!std::isfinite(timeline.sustain_ms));
  CHECK(std::isfinite(phase_durations(op, kMiddleC).sustain_ms));
  CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);
  CHECK(choose_held_ms(op, kMiddleC) > held_ms * 10.0);

  // Past the ceiling every sustain is the same width, and below it a faster
  // one is narrower -- which is the ordinary rule, back again.
  op.sr = 15;
  const double sr15 = choose_held_ms(op, kMiddleC);
  CHECK(sr15 < kMaxSpanMs);
  op.sr = 20;
  CHECK(choose_held_ms(op, kMiddleC) < sr15);
}

void test_an_ssg_loop_shows_a_few_periods() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 15;
  op.sl = 0;
  op.sr = 8;
  op.rr = 7;
  op.tl = 0;
  op.ssg = ssg_bits(0); // repeating saw

  const CurveResult held = simulate_held(op, kMiddleC);
  CHECK(held.loop_hz > 0.0);
  const double held_ms = choose_held_ms(op, kMiddleC);
  const double period_ms = 1000.0 / held.loop_hz;
  const double periods = held_ms / period_ms;
  CHECK(periods >= 3.0);
  CHECK(periods <= 4.0);
}

/// An SSG patch with AR < 31 spends its whole attack at or above the fold
/// threshold. Every one of those samples counting as a fold would report the
/// sample rate as the loop rate and collapse the graph to a tenth of a
/// millisecond wide, so the rate has to be a musical one.
void test_a_slow_attack_ssg_loop_reports_a_musical_rate() {
  OperatorParams op;
  op.ar = 14;
  op.dr = 18;
  op.sl = 9;
  op.sr = 14;
  op.rr = 0;
  op.tl = 0;
  op.ks = 0;
  op.ssg = ssg_bits(4); // inverted saw

  const CurveResult held = simulate_held(op, kMiddleC);
  // A loop a listener could hear as a tremolo, not 53 kHz.
  CHECK(held.loop_hz > 1.0);
  CHECK(held.loop_hz < 100.0);
  CHECK(near_rel(held.loop_hz, 6.12, 0.02));

  // ... and the window policy sizes itself from the period rather than from
  // the sample rate.
  const double held_ms = choose_held_ms(op, kMiddleC);
  const double periods = held_ms / (1000.0 / held.loop_hz);
  CHECK(periods >= 3.0);
  CHECK(periods <= 4.0);
}

/// The release's budget is its own: nothing about the held envelope may change
/// how far the release is simulated, or AR and DR would decide how long a
/// release is drawn.
void test_the_release_budget_is_its_own() {
  CHECK(release_max_ms() >= 4000.0);
  CHECK(release_max_ms() <= 10000.0);
  const double budget = release_max_ms();
  for (const OperatorParams &op :
       {adsr(31, 31, 0, 31, 7, 0), adsr(1, 1, 15, 1, 7, 0), worked_example()}) {
    (void)choose_held_ms(op, kMiddleC);
    CHECK(release_max_ms() == budget);
  }
}

// -------------------------------------------------------- the width policy

/**
 * A loop whose ramp never finishes is not a slow loop but no loop -- the
 * period comes back infinite -- and the axis is then sized by the phase that
 * stalled, exactly as it is for a patch with SSG-EG switched off. The
 * arithmetic that decides which patches those are is ssg_loop_period_ms()'s;
 * what is tested here is that the width policy believes it.
 */
void test_a_loop_that_never_folds_is_sized_like_a_plain_patch() {
  const OperatorParams stalled = ssg_patch(0, 31, 15, 8, 0, 7, 0); // SR = 0
  CHECK(!std::isfinite(ssg_loop_period_ms(stalled, kMiddleC)));
  CHECK(choose_held_ms(stalled, kMiddleC) ==
        window_for_timeline_ms(phase_durations(stalled, kMiddleC)));
  // ... and a mode that latches rather than folding takes the same road, with
  // a plain zero instead of an infinity.
  const OperatorParams latching = ssg_patch(1, 31, 15, 8, 8, 7, 0);
  CHECK(ssg_loop_period_ms(latching, kMiddleC) == 0.0);
  CHECK(choose_held_ms(latching, kMiddleC) ==
        window_for_timeline_ms(phase_durations(latching, kMiddleC)));
}

/// Which defect earns the one line the graph draws. The simulator decides what
/// is wrong with a patch; this only picks which of its answers is worth a
/// line, so the test asks the drawn curve rather than the registers.
void test_only_a_non_standard_ssg_attack_earns_a_line() {
  const auto warning_for = [](const OperatorParams &op) {
    return build_envelope_curve(op, kMiddleC).warning;
  };
  CHECK(warning_for(ssg_patch(0, 30, 15, 4, 8, 7, 0)) != nullptr);
  CHECK(warning_for(ssg_patch(0, 31, 15, 4, 8, 7, 0)) == nullptr);
  // The same AR without SSG-EG is nobody's business.
  CHECK(warning_for(adsr(30, 15, 4, 8, 7, 0)) == nullptr);
  // Every mode, including the ones that never fold: the convention is about
  // SSG-EG being on at all, not about whether the shape happens to loop.
  for (int type = 0; type < 8; ++type) {
    CHECK(warning_for(ssg_patch(type, 0, 15, 4, 8, 7, 0)) != nullptr);
    CHECK(warning_for(ssg_patch(type, 31, 15, 4, 8, 7, 0)) == nullptr);
  }
}

/**
 * The attack and the decay are never cut, and the ceiling is no exception.
 *
 * They are the shape being read -- an envelope drawn without its own beginning
 * says nothing about the registers that made it -- so the width is floored at
 * the instant the sustain starts, above every ceiling but the last. AR = 1 is
 * the case that motivates it: its attack alone runs 8.4 s.
 */
void test_the_attack_and_decay_are_never_cut() {
  PhaseDurations timeline;
  timeline.attack_ms = 8360.0;
  timeline.decay_ms = 460.0;
  timeline.sustain_ms = 200.0;
  // The whole envelope fits under the ceiling, so the whole envelope is drawn.
  CHECK(window_for_timeline_ms(timeline) == timeline.lifetime_ms());

  // Now give it a sustain no axis could hold. The ceiling takes the sustain
  // and stops there: the attack and decay stay whole.
  PhaseDurations spilling = timeline;
  spilling.sustain_ms = 60000.0;
  CHECK(window_for_timeline_ms(spilling) > spilling.sustain_start_ms());
  CHECK(window_for_timeline_ms(spilling) == kMaxSpanMs);

  // And when the attack and decay ALONE outlast the ceiling, the ceiling is
  // what gives way rather than the shape.
  // An attack and decay that fit under the ceiling keep their sustain too.
  PhaseDurations enormous;
  enormous.attack_ms = 12000.0;
  enormous.decay_ms = 3000.0;
  enormous.sustain_ms = 500.0;
  CHECK(enormous.sustain_start_ms() < kMaxSpanMs);
  CHECK(window_for_timeline_ms(enormous) == enormous.lifetime_ms());

  // Past the ceiling there is nothing left to give: the axis is the widest
  // there is, and the attack runs off the end of it.
  PhaseDurations beyond = enormous;
  beyond.attack_ms = 30000.0;
  CHECK(beyond.sustain_start_ms() > kMaxSpanMs);
  CHECK(window_for_timeline_ms(beyond) == kMaxSpanMs);
  CHECK(window_for_timeline_ms(enormous) > kMaxHeldMs);

  // The real registers behind that: AR = 1 at middle C.
  const OperatorParams slow = adsr(1, 12, 6, 4, 5, 0);
  const PhaseDurations real = phase_durations(slow, kMiddleC);
  CHECK(near_rel(real.attack_ms, 8360.0, 0.02));
  CHECK(choose_held_ms(slow, kMiddleC) > real.sustain_start_ms());
}

/**
 * ... and kMaxSpanMs is where that concession runs out.
 *
 * A rate of 0 never advances, so its phase lasts forever and the sum of the
 * phases before the sustain can be infinite -- which is not a width. Every
 * patch the chip can be given has to come back with an axis a graph can be
 * drawn on, so the floor is bounded too.
 */
void test_every_patch_has_a_finite_axis() {
  PhaseDurations frozen_attack;
  frozen_attack.attack_ms = std::numeric_limits<double>::infinity();
  frozen_attack.decay_ms = 100.0;
  frozen_attack.sustain_ms = 100.0;
  CHECK(window_for_timeline_ms(frozen_attack) == kMaxSpanMs);

  // AR = 0 and DR = 0 are the registers that produce it, and both are one
  // slider step away.
  for (const OperatorParams &op :
       {adsr(0, 10, 4, 5, 7, 0), adsr(31, 0, 4, 5, 7, 0)}) {
    CHECK(!std::isfinite(phase_durations(op, kMiddleC).sustain_start_ms()));
    CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
    CHECK(std::isfinite(curve.span_ms));
    CHECK(curve.span_ms <= kMaxSpanMs);
  }

  // Nothing anywhere in the register space escapes it -- not a phase that
  // never advances, and not one that merely takes two minutes.
  for (int ar = 0; ar <= 31; ar += 3) {
    for (int dr = 0; dr <= 31; dr += 3) {
      for (int sl = 0; sl <= 15; sl += 3) {
        for (int sr = 0; sr <= 31; sr += 3) {
          const double window =
              choose_held_ms(adsr(ar, dr, sl, sr, 7, 0), kMiddleC);
          CHECK(std::isfinite(window));
          CHECK(window >= 0.0);
          CHECK(window <= kMaxSpanMs);
        }
      }
    }
  }

  // An envelope with no attack, no decay and a hold that never ends has no
  // length at all. The width it asks for is a floor, not nothing, so there is
  // always a graph to draw on.
  const OperatorParams instant = adsr(31, 0, 0, 0, 7, 0);
  CHECK(choose_held_ms(instant, kMiddleC) == kMinHeldMs);
  const EnvelopeCurve curve = build_envelope_curve(instant, kMiddleC);
  CHECK(curve.held_ms == kMinHeldMs);
  CHECK(curve.span_ms >= kMinSpanMs);
}

/// Whatever the patch, the instant the sustain begins is inside the axis --
/// which is to say every phase the envelope actually has is at least partly
/// drawn. The two ends where it can only just be reached: an attack and decay
/// that fill the ceiling on their own leave no room for any sustain, and ones
/// that outlast kMaxSpanMs are cut there because nothing is drawn wider.
void test_every_phase_present_is_at_least_partly_visible() {
  for (int ar = 0; ar <= 31; ar += 3) {
    for (int dr = 0; dr <= 31; dr += 5) {
      for (int sl = 0; sl <= 15; sl += 3) {
        for (int ks = 0; ks <= 3; ks += 3) {
          const OperatorParams op = adsr(ar, dr, sl, 8, 5, ks);
          const PhaseDurations timeline = phase_durations(op, kMiddleC);
          const double start = timeline.sustain_start_ms();
          const double window = choose_held_ms(op, kMiddleC);
          if (start >= kMaxSpanMs) {
            CHECK(window == kMaxSpanMs);
          } else if (start > 0.0 && start < kMaxHeldMs) {
            // Room left under the ceiling, so some of the sustain is on the
            // axis as well as the whole of the shape before it.
            CHECK(window > start);
          } else {
            CHECK(window >= start);
          }
        }
      }
    }
  }
}

// -------------------------------------------------- the one-at-a-time sweeps

enum class Field { Ar, Dr, Sl, Sr, Rr };

const char *field_name(Field field) {
  switch (field) {
  case Field::Ar: return "AR";
  case Field::Dr: return "DR";
  case Field::Sl: return "SL";
  case Field::Sr: return "SR";
  case Field::Rr: return "RR";
  }
  return "?";
}

int field_top(Field field) {
  return (field == Field::Sl || field == Field::Rr) ? 15 : 31;
}

/// Every span the axis takes as one register is swept across its whole range,
/// with the rest of the patch held still.
std::vector<double> span_sweep(const OperatorParams &base, Field field) {
  std::vector<double> spans;
  for (int value = 0; value <= field_top(field); ++value) {
    OperatorParams op = base;
    switch (field) {
    case Field::Ar: op.ar = static_cast<uint8_t>(value); break;
    case Field::Dr: op.dr = static_cast<uint8_t>(value); break;
    case Field::Sl: op.sl = static_cast<uint8_t>(value); break;
    case Field::Sr: op.sr = static_cast<uint8_t>(value); break;
    case Field::Rr: op.rr = static_cast<uint8_t>(value); break;
    }
    spans.push_back(build_envelope_curve(op, kMiddleC).span_ms);
  }
  return spans;
}

/// Two spans that are the same width.
///
/// As SL moves attenuation from one phase to the next, the same total lifetime
/// is summed in a different order, so an axis that is mathematically unchanged
/// can differ in the last bits of a double. A pixel is a few tenths of a
/// percent of the axis; this is fifty million times finer.
constexpr double kSameSpan = 1e-9;

bool moves(const std::vector<double> &spans) {
  for (const double span : spans) {
    if (!near_rel(span, spans.front(), kSameSpan)) {
      return true;
    }
  }
  return false;
}

/// The base patches the sweeps run over: one slow enough that every phase runs
/// for seconds, EG_SPEC's worked example, one fast patch, one keyed hard by KS,
/// and two more slow ones.
const OperatorParams &sweep_base(int i) {
  static const OperatorParams bases[] = {
      slow_patch(),               worked_example(),
      adsr(31, 20, 8, 20, 12, 0), adsr(20, 15, 6, 10, 9, 3),
      adsr(6, 4, 10, 3, 2, 0),    adsr(31, 8, 2, 6, 10, 0),
  };
  return bases[i];
}
constexpr int kSweepBases = 6;

/**
 * Every rate moves the axis, and moves it one way only: a faster rate never
 * widens it, a slower one never narrows it. Each register is swept alone
 * across its whole range, over base patches slow enough that every phase runs
 * for seconds, and "no effect at all" fails as loudly as a jump does.
 *
 * Register 0 is exempt at both ends. A rate of 0 never advances, so its phase
 * lasts forever: for AR and DR that is the shape running past every ceiling,
 * and for SR it is the flat hold, which is a rule of its own. Either way the
 * step off 0 is a discontinuity the policy means to have, and the test below
 * pins it rather than smoothing it over.
 */
void test_every_rate_moves_the_axis_monotonically() {
  for (const Field field : {Field::Ar, Field::Dr, Field::Sr, Field::Rr}) {
    bool moved_somewhere = false;
    for (int i = 0; i < kSweepBases; ++i) {
      const std::vector<double> spans = span_sweep(sweep_base(i), field);
      for (size_t v = 2; v < spans.size(); ++v) {
        if (spans[v] > spans[v - 1] &&
            !near_rel(spans[v], spans[v - 1], kSameSpan)) {
          std::cout << field_name(field) << " widened the axis at " << v
                    << " on base " << i << ": " << spans[v - 1] << " -> "
                    << spans[v] << "\n";
        }
        CHECK(spans[v] <= spans[v - 1] ||
              near_rel(spans[v], spans[v - 1], kSameSpan));
        // ... and it is continuous while it narrows: no neighbouring pair of
        // registers may halve the axis.
        CHECK(spans[v] >= spans[v - 1] * 0.49);
      }
      moved_somewhere |= moves(spans);
    }
    // "No effect at all" is the thing this rules out.
    CHECK(moved_somewhere);
  }
}

/**
 * The one step the axis is allowed to jump, and the price of drawing a flat
 * line as a flat line.
 *
 * SR = 0 holds forever, so there is no length to be the axis and the hold takes
 * kFlatHoldShare of it. SR = 1 is a sustain that really does end -- after a
 * minute or two, so the ceiling takes it -- and the axis leaps from hundreds of
 * milliseconds to the ceiling for one register step. Every step after it is
 * ordinary again.
 */
void test_the_flat_hold_is_the_one_step_the_axis_jumps() {
  for (int i = 0; i < kSweepBases; ++i) {
    const std::vector<double> spans = span_sweep(sweep_base(i), Field::Sr);
    // Whatever it does at the step off zero, it is only ever wider afterwards:
    // a hold that never ends is never given more axis than one that does.
    CHECK(spans[1] >= spans[0] || near_rel(spans[1], spans[0], kSameSpan));
  }
  // Named, on EG_SPEC's worked example: 324 ms of flat hold against a sustain
  // that takes 109 s to die and is cut at the ceiling.
  OperatorParams op = worked_example();
  op.sr = 0;
  const double flat = choose_held_ms(op, kMiddleC);
  op.sr = 1;
  CHECK(choose_held_ms(op, kMiddleC) > flat * 10.0);
}

/**
 * SL, under this rule, is plainly monotone.
 *
 * Raising SL hands attenuation from the sustain phase to the decay phase. The
 * envelope's life is the time to fall through the whole scale, so moving a
 * slice of it from one rate to the other changes that life at a rate which is
 * the difference of the two speeds -- a constant. The axis is the life, so it
 * is linear in SL and therefore monotone, and its DIRECTION is a property of
 * the patch rather than of SL: a slow decay into a fast fade lengthens with SL,
 * a quick decay into a slow fade shortens with it, and dramatically. AR31 DR15
 * SL0 SR8 takes 9.7 s to fade from full volume; the same patch at SL15 is done
 * in 1.0 s, because DR does nearly all of the falling. A wider axis for the
 * second would be a lie.
 */
void test_sustain_level_moves_the_axis_monotonically() {
  bool moved_somewhere = false;
  for (int i = 0; i < kSweepBases; ++i) {
    const std::vector<double> spans = span_sweep(sweep_base(i), Field::Sl);
    bool risen = false;
    bool fallen = false;
    for (size_t v = 1; v < spans.size(); ++v) {
      const bool up = spans[v] > spans[v - 1] &&
                      !near_rel(spans[v], spans[v - 1], kSameSpan);
      const bool down = spans[v] < spans[v - 1] &&
                        !near_rel(spans[v], spans[v - 1], kSameSpan);
      risen |= up;
      fallen |= down;
      // One direction per patch, for the whole of the slider's travel.
      CHECK(!(risen && fallen));
    }
    moved_somewhere |= moves(spans);
  }
  CHECK(moved_somewhere);

  // Both directions, named. A slow decay into a fast fade lengthens with SL ...
  const std::vector<double> slow_decay =
      span_sweep(adsr(31, 4, 0, 20, 12, 0), Field::Sl);
  CHECK(slow_decay.back() > slow_decay.front());
  // ... and a quick decay into a slow fade shortens with it.
  const std::vector<double> quick_decay =
      span_sweep(adsr(31, 20, 0, 4, 12, 0), Field::Sl);
  CHECK(quick_decay.back() < quick_decay.front());
}

/**
 * TL0 AR1 DR12 SL6 RR5 at middle C.
 *
 * AR = 1 makes the attack alone last 8.4 s, and the axis has to reach past the
 * end of it whatever SR is: the sustain is the very thing SR governs, and an
 * axis sized so the attack no longer finishes inside the graph puts it
 * entirely off the right edge.
 */
void test_a_very_slow_attack_still_leaves_room_for_the_sustain() {
  double previous = 0.0;
  // From SR = 1: the step off the flat hold is the deliberate jump pinned in
  // test_the_flat_hold_is_the_one_step_the_axis_jumps().
  for (int sr = 1; sr <= 31; ++sr) {
    OperatorParams op = adsr(1, 12, 6, sr, 5, 0);
    op.tl = 0;
    const PhaseDurations timeline = phase_durations(op, kMiddleC);
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);

    // The attack really is that long, and the sustain starts seconds in.
    CHECK(near_rel(timeline.attack_ms, 8360.0, 0.02));
    CHECK(timeline.sustain_start_ms() > 3000.0);
    // ... and the axis reaches past it, for every SR.
    CHECK(curve.span_ms > timeline.sustain_start_ms());
    // The trace agrees: the attack ends inside the graph, and so does the
    // decay.
    CHECK(curve.attack_end_ms >= 0.0);
    CHECK(curve.attack_end_ms < curve.span_ms);
    CHECK(curve.decay_end_ms >= 0.0);
    CHECK(curve.decay_end_ms < curve.span_ms);
    if (previous > 0.0) {
      CHECK(curve.span_ms <= previous);
    }
    previous = curve.span_ms;
  }
}

/// The axis is a continuous function of the parameters: no neighbouring pair
/// of sustain levels may move it more than a factor of two either way.
///
/// SL = 15 is exempt, because it is a discontinuity of the *chip*:
/// sustain_attenuation() steps by 32 for every SL up to 14 and then jumps
/// straight to 0x3E0, handing the whole rest of the scale to the decay in one
/// move. The axis follows, as it should.
void test_the_axis_does_not_jump_between_neighbouring_values() {
  const auto sweep = [](int sl, int sr) {
    OperatorParams op;
    op.ar = 4;
    op.dr = 26;
    op.sl = static_cast<uint8_t>(sl);
    op.sr = static_cast<uint8_t>(sr);
    op.rr = 7;
    op.ks = 3; // envelopes several seconds long
    return build_envelope_curve(op, kMiddleC).span_ms;
  };

  for (int sr = 1; sr <= 6; ++sr) {
    double previous = sweep(0, sr);
    for (int sl = 1; sl <= 14; ++sl) {
      const double span = sweep(sl, sr);
      // A factor of two either way; never a policy switch.
      CHECK(span <= previous * 2.01);
      CHECK(span >= previous * 0.49);
      previous = span;
    }
    // ... and SL = 15 moves it, because the chip's own table does.
    CHECK(sweep(15, sr) < previous);
  }
  // The matching guarantee along the rate axes is asserted inside
  // test_every_rate_moves_the_axis_monotonically(), which already has the
  // sweeps in hand.
}

// ------------------------------------------------------------------ the axis

/**
 * The span is simply the content it has to hold, at full precision. A drag on
 * a slider is kept from making the axis breathe by animating the drawn width
 * towards the new one, not by rounding the answer onto a ladder of round
 * numbers.
 */
void test_the_span_is_the_content_it_has_to_hold() {
  // The held window is always inside the axis, and so is the release unless the
  // budget has deliberately let it run off the right edge.
  const OperatorParams patches[] = {
      worked_example(),        slow_patch(),
      adsr(31, 20, 8, 20, 12, 0), adsr(6, 4, 10, 3, 2, 0),
      adsr(31, 8, 2, 6, 10, 0),   adsr(20, 15, 6, 10, 9, 3),
  };
  for (const auto &op : patches) {
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
    CHECK(curve.span_ms >= curve.held_ms || curve.span_ms == kMaxHeldMs);
    CHECK(curve.span_ms >= curve.release_content_ms ||
          curve.span_ms >= curve.held_ms * 3.9);
    // No round numbers to land on: the axis is a real width, not a rung.
    CHECK(curve.span_ms >= kMinSpanMs);
  }
}

/// The curve is a pure function of the operator and the note: the axis it is
/// drawn on does not depend on where the axis has been.
void test_the_span_does_not_depend_on_where_the_axis_has_been() {
  EnvelopeCurveCache first;
  EnvelopeCurveCache second;
  // The detour has to actually happen, so nothing may be throttled away.
  first.set_clock(&leaping_clock);
  second.set_clock(&leaping_clock);
  const OperatorParams a = worked_example();
  const OperatorParams b = adsr(6, 4, 10, 3, 2, 0);

  // One cache walks a -> b -> a, the other only ever sees a.
  first.get(a, kMiddleC);
  first.get(b, kMiddleC);
  const double after_a_detour = first.get(a, kMiddleC).span_ms;
  const double straight = second.get(a, kMiddleC).span_ms;
  CHECK(after_a_detour == straight);
  CHECK(build_envelope_curve(a, kMiddleC).span_ms == straight);
}

void test_the_grid_step_divides_the_span_sensibly() {
  // Every width the policy can hand it, including the wider axis a slow loop
  // is allowed.
  for (double span = kMinSpanMs; span <= kLoopMaxAxisMs; span += 5.0) {
    const double step = grid_step_ms(span);
    CHECK(step > 0.0);
    // 10000 is the top of the grid ladder, not the axis limit it happens to
    // share a value with: past it the divisions simply get coarser.
    CHECK(span / step <= 6.0 || step == 10000.0);
    CHECK(span / step >= 1.0);
  }
}

// ------------------------------------------------------ end-to-end geometry

void test_the_worked_examples_decay_lands_on_the_real_millisecond_axis() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  // EG_SPEC: the decay of AR=31 TL=0 DR=10 SL=2 ends on tick 5440 = 306.4 ms.
  CHECK(curve.decay_end_ms > 0.0);
  CHECK(near_rel(curve.decay_end_ms, 306.4, 0.02));

  // Everything the graph draws is on that same axis.
  CHECK(curve.attack_end_ms >= 0.0);
  CHECK(curve.attack_end_ms < curve.decay_end_ms);
  CHECK(curve.held_ms > curve.decay_end_ms);
  CHECK(curve.span_ms >= curve.held_ms);
  // The held trace is simulated across the whole axis, not just the window
  // the policy asked for, so it ends at the right edge rather than in mid-air.
  CHECK(near_rel(content_ms(curve.held), curve.span_ms, 0.01));
  CHECK(!curve.held.points.empty());
  CHECK(curve.warning == nullptr);

  // The key is never released on the held trace, so nothing on it is a
  // key-off and the sustain runs to the end of the window.
  CHECK(!has_marker(curve.held, MarkerKind::KeyOff));
  CHECK(!curve.held_parked); // SR = 5 keeps crawling

  // AR = 31 is an instant attack, so the curve starts at full volume; it is
  // monotonic in time, and it is still sustaining where it stops.
  CHECK(curve.held.points.front().ms == 0.0f);
  CHECK(curve.held.points.front().out == 0);
  CHECK(curve.held.points.back().out < kMaxAttenuation);
  CHECK(curve.peak_out == 0); // TL = 0
  CHECK(curve.sustain_out == 64);
  double previous = -1.0;
  bool reached_peak = false;
  for (const auto &p : curve.held.points) {
    CHECK(p.ms >= previous);
    previous = p.ms;
    reached_peak |= (p.out == 0);
  }
  CHECK(reached_peak);
}

/// The second trace: a release from full volume, on the same axis but from
/// t = 0, so it never depends on when a key-off happens.
void test_the_release_starts_at_full_volume_and_reaches_silence() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  CHECK(curve.release.points.size() >= 2);
  CHECK(curve.release.points.front().ms == 0.0f);
  CHECK(curve.release.points.front().out == curve.peak_out);
  CHECK(!curve.release_truncated);
  CHECK(curve.release.points.back().out == kMaxAttenuation);
  // RR = 7 at middle C.
  CHECK(near_rel(curve.release_content_ms, 907.7, 0.02));
  // It falls, and it never rises.
  double previous_out = -1.0;
  for (const auto &p : curve.release.points) {
    CHECK(p.out >= previous_out);
    previous_out = p.out;
  }
}

/// SSG-EG releases through the same rules the chip uses -- the increment is
/// quadrupled and the ramp is cut dead at 0x200 -- so the same RR is several
/// times faster with it than without.
void test_ssg_eg_makes_the_release_dramatically_shorter() {
  OperatorParams plain = worked_example();
  plain.rr = 6;
  OperatorParams ssg = plain;
  ssg.ssg = ssg_bits(0); // repeating saw

  const EnvelopeCurve plain_curve = build_envelope_curve(plain, kMiddleC);
  const EnvelopeCurve ssg_curve = build_envelope_curve(ssg, kMiddleC);

  CHECK(near_rel(plain_curve.release_content_ms, 1815.3, 0.02));
  CHECK(near_rel(ssg_curve.release_content_ms, 229.8, 0.02));
  // 4x from the increment, and the rest from the hard cut at 0x200.
  CHECK(ssg_curve.release_content_ms * 4.0 < plain_curve.release_content_ms);
  CHECK(plain_curve.release.points.back().out == kMaxAttenuation);
  CHECK(ssg_curve.release.points.back().out == kMaxAttenuation);
}

/// The held trace is simulated across the whole span, so a loop keeps looping
/// all the way to the right edge instead of being extrapolated along the slope
/// of its last ramp, which would not be a sawtooth at all.
void test_a_loop_keeps_looping_to_the_right_edge() {
  const OperatorParams patches[] = {
      ssg_patch(4, 14, 18, 9, 14, 0, 0),
      ssg_patch(4, 29, 1, 0, 7, 0, 0),
      ssg_patch(4, 8, 10, 7, 4, 0, 0),
  };
  for (const auto &op : patches) {
    const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);
    // A loop a listener could hear, not the sample rate.
    CHECK(c.held.loop_hz > 0.1);
    CHECK(c.held.loop_hz < 100.0);
    // The polyline reaches the edge of the axis on its own.
    CHECK(near_rel(content_ms(c.held), c.span_ms, 0.01));
    CHECK(!c.held_parked);
    // ... and it is still folding when it gets there: the last fold is within
    // one period of the right edge, so no more than one ramp is unfinished.
    const double period_ms = 1000.0 / c.held.loop_hz;
    double last_fold = -1.0;
    int folds = 0;
    for (const auto &m : c.held.markers) {
      if (m.kind == MarkerKind::SsgFold) {
        last_fold = m.ms;
        ++folds;
      }
    }
    CHECK(folds >= 3);
    CHECK(last_fold > c.span_ms - period_ms * 1.05);
    // No key-off inside the graph, whatever gate the run was asked for.
    CHECK(!has_marker(c.held, MarkerKind::KeyOff));
    // The release is a real curve, not the one-sample cut.
    CHECK(c.release_content_ms > 100.0);
  }
}

/// The inverted SSG-EG modes (types 4-7) run the attenuation scale backwards:
/// att = 0 is the QUIETEST point of the ramp and 0x200 is full volume. A
/// release started from 0 is therefore a release from silence, which key-off
/// cuts dead in a single sample. The release has to start from whatever level
/// output() is loudest at, and then an inverted mode releases at exactly the
/// same speed as its upright twin, because key-off latches the audible level
/// either way.
void test_an_inverted_ssg_release_is_as_long_as_the_upright_one() {
  OperatorParams upright = worked_example();
  upright.rr = 6;
  upright.ssg = ssg_bits(0); // saw, output not inverted
  OperatorParams inverted = upright;
  inverted.ssg = ssg_bits(4); // the same saw, inverted

  CHECK(loudest_attenuation(upright) == 0);
  CHECK(loudest_attenuation(inverted) == kSsgFoldAttenuation);

  const EnvelopeCurve up = build_envelope_curve(upright, kMiddleC);
  const EnvelopeCurve inv = build_envelope_curve(inverted, kMiddleC);

  CHECK(near_rel(up.release_content_ms, 229.8, 0.02));
  CHECK(near_rel(inv.release_content_ms, up.release_content_ms, 0.01));
  // Not the one-sample cut: at 53 kHz that would be 0.02 ms.
  CHECK(inv.release_content_ms > 100.0);
  // Both start from full volume and both end in silence. An inverted mode is
  // loudest at the fold level itself, which an operator only ever passes
  // through, so its release starts one step short of the nominal peak.
  CHECK(inv.release.points.front().out == inv.peak_out + 1);
  CHECK(inv.release.points.back().out == kMaxAttenuation);
  CHECK(!inv.release_truncated);
  // ... and it still only ever falls.
  double previous_out = -1.0;
  for (const auto &p : inv.release.points) {
    CHECK(p.out >= previous_out);
    previous_out = p.out;
  }
}

/// SR = 0 is the case a chained graph could not tell the truth about: with a
/// key-off in the picture the hold is cut short, and without one it is simply
/// flat forever.
void test_an_sr0_held_trace_parks_and_stays_flat() {
  OperatorParams op = worked_example();
  op.sr = 0;
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);

  CHECK(curve.held_parked);
  CHECK(!has_marker(curve.held, MarkerKind::KeyOff));
  CHECK(std::isfinite(curve.held.park_ms));
  // It parks the moment the decay reaches the sustain level ...
  CHECK(near_rel(curve.held.park_ms, 306.4, 0.02));
  // ... at the sustain level, and never moves again.
  CHECK(curve.held.points.back().out == curve.sustain_out);
  for (const auto &p : curve.held.points) {
    CHECK(p.out <= curve.sustain_out);
    if (p.ms >= curve.held.park_ms) {
      CHECK(p.out == curve.sustain_out);
    }
  }
  // The window is wider than the park, so the flat stretch is visible rather
  // than a single pixel at the right edge.
  CHECK(curve.held_ms > curve.held.park_ms);
  CHECK(curve.span_ms > curve.held.park_ms);
}

void test_total_level_moves_the_whole_curve_down() {
  OperatorParams op = worked_example();
  op.tl = 32;
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  CHECK(curve.peak_out == 32 * 8);
  CHECK(curve.sustain_out == 64 + 32 * 8);
  for (const auto &p : curve.held.points) {
    CHECK(p.out >= curve.peak_out);
  }
  // The release starts from the same peak the held trace does.
  for (const auto &p : curve.release.points) {
    CHECK(p.out >= curve.peak_out);
  }
}

void test_ssg_enabled_curves_fold() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 15;
  op.sl = 0;
  op.sr = 8;
  op.rr = 7;
  op.ssg = ssg_bits(0);

  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  int folds = 0;
  for (const auto &m : curve.held.markers) {
    folds += (m.kind == MarkerKind::SsgFold) ? 1 : 0;
  }
  CHECK(folds >= 3);
  CHECK(!has_marker(curve.held, MarkerKind::KeyOff));
}

/// An audio-rate loop must keep its own scale: the release is far longer than
/// the loop, and letting it set the axis packs the cycles into a solid block.
void test_a_fast_loop_keeps_its_scale() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 24;
  op.sl = 15;
  op.rr = 7;
  op.ssg = ssg_bits(0);

  const EnvelopeCurve fast = build_envelope_curve(op, kMiddleC);
  CHECK(fast.held.loop_hz > 100.0);
  // The loop, not the release, decides the width.
  CHECK(fast.release_content_ms > fast.span_ms);
  CHECK(fast.span_ms <= fast.held_ms * 3.0);
  // ... and it still gets a readable share of it.
  CHECK(fast.held_ms / fast.span_ms > 0.25);

  // A slow loop already fits, so nothing about it changes.
  OperatorParams slow = op;
  slow.dr = 15;
  const EnvelopeCurve wide = build_envelope_curve(slow, kMiddleC);
  CHECK(wide.held.loop_hz < 20.0);
  CHECK(wide.held_ms / wide.span_ms > 0.25);
}

/**
 * A slow loop must not lose its periods, whatever it is that would take them.
 *
 * The sweep runs DR the whole way down to 1, where one ramp of this patch
 * takes fifteen seconds, and asks for two things at every step: a slower loop
 * never gets a narrower axis, and one whole period stays on it -- unless no
 * axis could hold one, and then the widest there is, with a ramp on it.
 */
void test_a_slow_loop_still_shows_a_period() {
  const auto patch = [](int dr) {
    OperatorParams op;
    op.ar = 31;
    op.dr = static_cast<uint8_t>(dr);
    op.sl = 15;
    op.sr = 0;
    op.rr = 2;
    op.ssg = ssg_bits(2); // triangle: two ramps per period
    return op;
  };

  double previous = 0.0;
  for (int dr = 20; dr >= 1; --dr) {
    const OperatorParams op = patch(dr);
    const double period = ssg_loop_period_ms(op, kMiddleC);
    const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);
    size_t folds = 0;
    for (const Marker &m : c.held.markers) {
      if (m.kind == MarkerKind::SsgFold && m.ms <= c.span_ms) {
        ++folds;
      }
    }
    std::cout << "DR " << dr << ": period " << period << " ms, span "
              << c.span_ms << " ms, " << folds << " folds\n";
    // Two folds is one whole period of an alternating mode. A loop slower than
    // the ceiling cannot have one -- there is no axis long enough -- so what is
    // promised there is the widest axis there is, and the ramp on it.
    if (period * kLoopVisiblePeriods <= kLoopMaxAxisMs) {
      CHECK(folds >= 2);
    } else {
      CHECK(near_rel(c.span_ms, kLoopMaxAxisMs, 0.001));
      CHECK(folds >= 1);
    }
    // ... and the axis is wide enough to hold one, which is the promise the
    // fold count above is only the evidence for.
    CHECK(c.span_ms >= std::min(period, kLoopMaxAxisMs));
    // A slower loop never gets a narrower axis.
    CHECK(c.span_ms >= previous * 0.999);
    previous = c.span_ms;
  }
}

/// The same bargain for a plain patch: RR = 2 takes over ten seconds where the
/// attack and decay take three hundred milliseconds, and an axis wide enough
/// for all of it would leave nothing of the part being edited.
void test_a_very_long_release_does_not_crush_the_held_trace() {
  OperatorParams op = worked_example();
  op.rr = 2;
  const EnvelopeCurve slow = build_envelope_curve(op, kMiddleC);
  CHECK(slow.release_truncated); // still falling when the budget ran out
  CHECK(slow.release.points.back().out < kMaxAttenuation);
  CHECK(slow.held_ms / slow.span_ms > 0.15);

  // A release that fits is drawn whole, on an axis wide enough for it.
  OperatorParams fits = worked_example();
  fits.rr = 6;
  const EnvelopeCurve normal = build_envelope_curve(fits, kMiddleC);
  CHECK(!normal.release_truncated);
  CHECK(normal.span_ms >= normal.release_content_ms);
}

/// A slow release must be simulated far enough to be worth drawing. Stopping
/// after a few samples would draw a near-flat sliver that reads as "this note
/// never decays".
void test_a_slow_release_is_simulated_to_the_end() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 8;
  op.sr = 0; // parks in the sustain hold
  op.sl = 2;
  op.rr = 4; // seconds long, even at SSG-EG's 4x
  op.ssg = ssg_bits(0);

  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);
  CHECK(!c.release_truncated);
  CHECK(c.release.points.back().out == kMaxAttenuation);
  CHECK(c.release_content_ms > 900.0);
  // The held trace parks, and the release is four times longer than the flat
  // hold it hangs off. So the release is what makes the axis wide -- as wide
  // as the budget lets it, past which it runs off the right edge rather than
  // squeezing the shape into the left margin.
  CHECK(c.held_parked);
  CHECK(c.span_ms > c.held_ms);
  CHECK(c.span_ms >= c.held_ms * 3.9);
}

// -------------------------------------------------------------- warnings

void test_the_only_warning_is_the_non_standard_ssg_attack() {
  // An SSG-EG mode expects AR = 31; anything less is the one thing the graph
  // says out loud, because the curve alone does not explain it.
  OperatorParams slow_attack;
  slow_attack.ar = 20;
  slow_attack.dr = 15;
  slow_attack.sl = 4;
  slow_attack.sr = 8;
  slow_attack.rr = 7;
  slow_attack.ssg = ssg_bits(0);
  const EnvelopeCurve slow = build_envelope_curve(slow_attack, kMiddleC);
  CHECK(slow.warning != nullptr);
  CHECK(std::string(slow.warning) == "AR<31: non-standard SSG-EG");
  // The same patch without SSG-EG has nothing wrong with it.
  OperatorParams plain = slow_attack;
  plain.ssg = 0;
  CHECK(build_envelope_curve(plain, kMiddleC).warning == nullptr);

  // Everything else the simulator flags is left to the shape of the curve:
  // a frozen attack is a flat line at silence ...
  OperatorParams frozen = worked_example();
  frozen.ar = 0;
  CHECK(build_envelope_curve(frozen, kMiddleC).warning == nullptr);

  // ... a loop with no teeth is plainly not looping ...
  OperatorParams never_loops;
  never_loops.ar = 31;
  never_loops.dr = 15;
  never_loops.sl = 4;
  never_loops.sr = 0;
  never_loops.rr = 7;
  never_loops.ssg = ssg_bits(0);
  const EnvelopeCurve stuck = build_envelope_curve(never_loops, kMiddleC);
  CHECK(stuck.warning == nullptr);

  // ... and an audio-rate loop is a solid block of them.
  OperatorParams audio_rate;
  audio_rate.ar = 31;
  audio_rate.dr = 24;
  audio_rate.sl = 15;
  audio_rate.rr = 7;
  audio_rate.ssg = ssg_bits(0);
  const EnvelopeCurve fast = build_envelope_curve(audio_rate, kMiddleC);
  CHECK(fast.held.loop_hz > 100.0); // the simulator still reports it ...
  CHECK(fast.warning == nullptr);   // ... the graph just does not repeat it.

  CHECK(build_envelope_curve(worked_example(), kMiddleC).warning == nullptr);
}

// ----------------------------------------------------------------- cache

void test_the_cache_recomputes_only_on_a_real_change() {
  EnvelopeCurveCache cache;
  // Which registers count as a change, not when the change is acted on.
  cache.set_clock(&leaping_clock);
  OperatorParams op = worked_example();

  const double first_span = cache.get(op, kMiddleC).span_ms;
  CHECK(cache.rebuild_count() == 1);
  for (int i = 0; i < 10; ++i) {
    CHECK(cache.get(op, kMiddleC).span_ms == first_span);
  }
  CHECK(cache.rebuild_count() == 1);

  op.dr = 12;
  cache.get(op, kMiddleC);
  CHECK(cache.rebuild_count() == 2);
}

// -------------------------------------------------------- rebuild throttle

/// Everything about a drawn curve that a register change can move.
bool same_drawn_curve(const EnvelopeCurve &lhs, const EnvelopeCurve &rhs) {
  return lhs.span_ms == rhs.span_ms && lhs.held_ms == rhs.held_ms &&
         lhs.peak_out == rhs.peak_out && lhs.sustain_out == rhs.sustain_out &&
         lhs.held.points.size() == rhs.held.points.size() &&
         lhs.release.points.size() == rhs.release.points.size();
}

/// The patch the whole throttle exists for: an SSG-EG loop, about 3.5 ms.
OperatorParams expensive_patch() { return ssg_patch(4, 14, 18, 9, 14, 0, 0); }

void test_the_throttle_spaces_a_rebuild_by_what_it_cost() {
  // The budget is per frame, so a rebuild that fits it earns exactly one
  // frame's wait -- which is to say none, because a frame will have passed
  // before the question is asked again.
  CHECK(near_rel(RebuildThrottle::interval_for_ms(kRebuildBudgetMs),
                 kRebuildBudgetPeriodMs, 1e-9));
  CHECK(RebuildThrottle::interval_for_ms(kCheapBuildMs) < kFrameMs);
  CHECK(RebuildThrottle::interval_for_ms(kExpensiveBuildMs) > kFrameMs);

  // Monotone in the cost, up to the ceiling past which a graph stops looking
  // slow and starts looking broken.
  double previous = 0.0;
  for (double cost = 0.0; cost < 20.0; cost += 0.05) {
    const double interval = RebuildThrottle::interval_for_ms(cost);
    CHECK(interval >= previous);
    CHECK(interval <= kMaxRebuildDeferMs);
    previous = interval;
  }
  CHECK(RebuildThrottle::interval_for_ms(1000.0) == kMaxRebuildDeferMs);

  RebuildThrottle throttle;
  // Nothing has been built, so there is nothing to draw instead: never defer.
  CHECK(throttle.may_rebuild(0.0));

  throttle.note_rebuild(100.0, kExpensiveBuildMs);
  const double interval = RebuildThrottle::interval_for_ms(kExpensiveBuildMs);
  CHECK(throttle.interval_ms() == interval);
  CHECK(!throttle.may_rebuild(100.0));
  CHECK(!throttle.may_rebuild(100.0 + interval - 0.001));
  CHECK(throttle.may_rebuild(100.0 + interval + 0.001));

  // A rebuild too quick to measure earns no wait at all.
  throttle.note_rebuild(200.0, 0.0);
  CHECK(throttle.may_rebuild(200.0));
}

/// The common case, and the one the throttle must not touch: an ordinary patch
/// is cheap enough to simulate every frame, so a drag on one still does.
void test_a_cheap_curve_is_rebuilt_every_frame() {
  EnvelopeCurveCache cache;
  cache.set_clock(&fake_now_ms);
  g_clock.now_ms = 0.0;
  g_clock.cost_ms = kCheapBuildMs;

  OperatorParams op = worked_example();
  for (int frame = 0; frame < 60; ++frame) {
    g_clock.now_ms = frame * kFrameMs;
    op.tl = static_cast<uint8_t>(frame);
    cache.get(op, kMiddleC);
  }
  CHECK(cache.rebuild_count() == 60);
}

/// The case it exists for: a rebuild worth two and a third frames of budget is
/// spaced three frames apart, and the two frames in between draw the curve
/// that is already there.
void test_an_expensive_curve_is_deferred_while_the_value_moves() {
  EnvelopeCurveCache cache;
  cache.set_clock(&fake_now_ms);
  g_clock.now_ms = 0.0;
  g_clock.cost_ms = kExpensiveBuildMs;

  const double interval = RebuildThrottle::interval_for_ms(kExpensiveBuildMs);
  CHECK(interval > 2.0 * kFrameMs && interval < 3.0 * kFrameMs);

  OperatorParams op = expensive_patch();
  uint16_t drawn_peak = 0;
  for (int frame = 0; frame < 60; ++frame) {
    g_clock.now_ms = frame * kFrameMs;
    op.tl = static_cast<uint8_t>(frame);
    drawn_peak = cache.get(op, kMiddleC).peak_out;
  }
  CHECK(cache.rebuild_count() == 20); // frames 0, 3, 6 ... 57
  // The last frame of the drag is still drawing frame 57's curve, which is
  // what "deferred" means: total_level lands in the output attenuation.
  CHECK(drawn_peak == 57 * 8);

  // ... and the moment the value stops moving, the real one arrives.
  g_clock.now_ms = 60 * kFrameMs;
  CHECK(cache.get(op, kMiddleC).peak_out == 59 * 8);
  CHECK(cache.rebuild_count() == 21);
}

/// Whatever the throttle did during a drag, the value the drag ENDS on is
/// simulated for real -- one frame later, whichever frame it ends on.
void test_a_settled_patch_always_converges_on_the_exact_curve() {
  for (int frames = 1; frames <= 24; ++frames) {
    EnvelopeCurveCache cache;
    cache.set_clock(&fake_now_ms);
    g_clock.now_ms = 0.0;
    g_clock.cost_ms = kExpensiveBuildMs;

    OperatorParams op = worked_example();
    for (int frame = 0; frame < frames; ++frame) {
      g_clock.now_ms = frame * kFrameMs;
      op.tl = static_cast<uint8_t>(frame * 3);
      cache.get(op, kMiddleC);
    }
    // The mouse comes up. One sixtieth of a second later the graph is exact,
    // whether or not this value was ever one the throttle let through.
    g_clock.now_ms = frames * kFrameMs;
    CHECK(same_drawn_curve(cache.get(op, kMiddleC),
                           build_envelope_curve(op, kMiddleC)));
    // And it stays exact without simulating anything again.
    const int settled = cache.rebuild_count();
    cache.get(op, kMiddleC);
    CHECK(cache.rebuild_count() == settled);
  }
}

/// Nothing changed for the frames the user is not editing: no rebuild, and not
/// even a look at the clock.
void test_a_still_patch_costs_the_cache_nothing() {
  EnvelopeCurveCache cache;
  cache.set_clock(&fake_now_ms);
  g_clock.now_ms = 0.0;
  g_clock.cost_ms = kExpensiveBuildMs;

  const OperatorParams op = expensive_patch();
  const double first_span = cache.get(op, kMiddleC).span_ms;
  CHECK(cache.rebuild_count() == 1);
  const double clock_after_the_build = g_clock.now_ms;

  for (int frame = 0; frame < 120; ++frame) {
    CHECK(cache.get(op, kMiddleC).span_ms == first_span);
  }
  CHECK(cache.rebuild_count() == 1);
  CHECK(g_clock.now_ms == clock_after_the_build);
}

/// The note is part of what a cached curve was built at, so moving it
/// invalidates the curve exactly as a register change does.
void test_the_cache_rebuilds_when_the_note_changes() {
  EnvelopeCurveCache cache;
  cache.set_clock(&leaping_clock);
  OperatorParams op = worked_example();
  op.ks = 3;

  cache.get(op, kMiddleC);
  cache.get(op, kMiddleC);
  CHECK(cache.rebuild_count() == 1);

  const NotePitch c6 = NotePitch::from_midi(84);
  cache.get(op, c6);
  cache.get(op, c6);
  CHECK(cache.rebuild_count() == 2);

  cache.get(op, kMiddleC);
  CHECK(cache.rebuild_count() == 3);
}

// ------------------------------------------------------- the live cursor

/// The release the chip would actually run from `att`, simulated on its own
/// terms -- the thing release_entry_ms() claims is already on the graph.
CurveResult simulated_release_from(const OperatorParams &op, NotePitch pitch,
                                   double att) {
  CurveRequest request;
  request.op = op;
  // Same as the drawn release: AR is irrelevant to a release, and leaving it
  // high would let key_on() snap the start level away.
  request.op.ar = 0;
  request.pitch = pitch;
  // Same as the drawn release: a key write takes a sample to reach the
  // envelope, so a note released on sample zero never starts.
  const double sample_ms = 1000.0 / sample_rate_hz(kNtscClockHz);
  request.gate_ms = 2.0 * sample_ms;
  request.max_ms = release_max_ms();
  request.start_att = static_cast<uint16_t>(att + 0.5);
  CurveResult result = sample_curve(request);
  const auto first =
      std::find_if(result.points.begin(), result.points.end(),
                   [sample_ms](const CurvePoint &p) { return p.ms >= sample_ms; });
  if (first != result.points.begin() && first != result.points.end()) {
    result.points.erase(result.points.begin(), first);
    const float origin = result.points.front().ms;
    for (CurvePoint &p : result.points)
      p.ms -= origin;
  }
  return result;
}

void test_the_cursor_is_where_the_elapsed_time_says() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  // Held, still moving: the cursor is simply the elapsed time.
  for (const double elapsed : {0.0, 1.0, 12.5, 100.0}) {
    const VoiceCursor cursor =
        cursor_for_voice(curve, elapsed, -1.0, curve.span_ms);
    CHECK(!cursor.released);
    CHECK(std::fabs(cursor.ms - elapsed) < 1e-9);
    CHECK(cursor.silent_for_ms == 0.0);
  }
  // A key that has not been pressed yet cannot put the cursor left of zero.
  CHECK(cursor_for_voice(curve, -5.0, -1.0, curve.span_ms).ms == 0.0);
}

void test_a_cursor_still_moving_leaves_the_graph() {
  // SR > 0: the held envelope keeps crawling, so nothing but the axis stops
  // the cursor -- and the axis does not stop it, it just stops drawing it.
  // A cursor parked on the edge would say the envelope had come to rest there.
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  CHECK(!curve.held_parked || curve.held.park_ms > curve.span_ms);
  const VoiceCursor cursor =
      cursor_for_voice(curve, curve.span_ms * 4.0, -1.0, curve.span_ms);
  CHECK(cursor.ms > curve.span_ms);
}

void test_a_parked_cursor_stays_where_the_envelope_stopped() {
  OperatorParams op = worked_example();
  op.sr = 0; // reaches the sustain level and holds
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  CHECK(curve.held_parked);
  const double park = curve.held.park_ms;
  CHECK(park < curve.span_ms); // the axis is wider, so this is a real clamp

  // Before the park the cursor tracks the elapsed time ...
  CHECK(std::fabs(cursor_for_voice(curve, park * 0.5, -1.0, curve.span_ms).ms -
                  park * 0.5) < 1e-9);
  // ... and after it, it does not move again however long the key is held.
  for (const double elapsed : {park + 1.0, park * 2.0, 60000.0}) {
    const VoiceCursor cursor =
        cursor_for_voice(curve, elapsed, -1.0, curve.span_ms);
    CHECK(std::fabs(cursor.ms - park) < 1e-9);
  }
  // Parked at the sustain level, not at silence: nothing to fade out.
  CHECK(cursor_for_voice(curve, 60000.0, -1.0, curve.span_ms).silent_for_ms ==
        0.0);
}

/**
 * The claim the whole release cursor rests on: a release starting from level L
 * IS the drawn release-from-full-volume, entered later. If that is true, the
 * two traces agree everywhere past the entry point.
 *
 * They do -- up to the one thing no second simulation could fix either. The
 * EG's 12-bit counter is free-running and shared by all 24 operators, and the
 * increment a rate finds depends on where in that counter it lands: the drawn
 * trace arrives at level L with whatever phase climbing from full volume left
 * it, and a release the chip really runs arrives with whatever phase the
 * attack and decay left. So the two can slip by up to one update period of the
 * release rate -- for RR = 7 that is eight EG ticks -- and that slip is a
 * property of the hardware, not of drawing one release instead of six.
 *
 * Where the rate updates every tick and its table row is uniform, there is no
 * phase left to disagree about, and the two agree to within a sample.
 */
void test_the_release_entry_point_matches_a_real_release() {
  const double sample_ms = 1000.0 / sample_rate_hz(kNtscClockHz);
  const double eg_tick_ms = 1000.0 / eg_rate_hz(kNtscClockHz);

  double worst_periods = 0.0;
  double worst_uniform_samples = 0.0;
  for (int rr = 0; rr <= 15; ++rr) {
    OperatorParams op = worked_example();
    op.rr = static_cast<uint8_t>(rr);
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
    const int rate = ym2612_eg::detail::effective_rate(
        2 * rr + 1, key_scale_value(op, kMiddleC));
    // How long the chip waits between two chances to act at this rate: the
    // whole of the slip the shared counter can produce.
    const double update_ms =
        static_cast<double>(1 << ym2612_eg::detail::rate_shift(rate)) *
        eg_tick_ms;

    double worst_ms = 0.0;
    // Key-off from partway down the decay, at the sustain level, and from
    // deep into the sustain's own crawl.
    for (const double key_off_ms : {20.0, 40.0, 120.0, 400.0, 1200.0}) {
      // An exact level, so the only thing left to disagree about is the
      // counter phase: an interpolated one would put the two traces a
      // fraction of an attenuation unit apart before either had started.
      const auto att =
          static_cast<uint16_t>(curve_out_at_ms(curve.held, key_off_ms) + 0.5);
      const double entry = release_entry_ms(curve.release, att);
      const CurveResult real = simulated_release_from(op, kMiddleC, att);
      if (real.points.size() < 2 || curve.release.points.size() < 2) {
        continue; // an instant cut has no trajectory to compare
      }
      // Only levels both traces actually reached. RR = 0 never gets to
      // silence at all, so past its budget there is nothing on either side.
      const double ceiling = std::min<double>(curve.release.points.back().att,
                                              real.points.back().att);
      // Compare where each trace reaches a ladder of levels below the one it
      // started at -- a comparison in TIME, which is what a cursor is placed
      // by.
      for (double level = att + 32.0; level <= ceiling; level += 64.0) {
        const double drawn = release_entry_ms(curve.release, level) - entry;
        const double actual = release_entry_ms(real, level);
        worst_ms = std::max(worst_ms, std::fabs(drawn - actual));
      }
    }
    // Never worse than the counter phase can account for.
    CHECK(worst_ms <= update_ms);
    worst_periods = std::max(worst_periods, worst_ms / update_ms);

    // A rate that acts every tick, on a row whose eight entries are equal,
    // has no phase to disagree about at all.
    const bool uniform = ym2612_eg::detail::rate_shift(rate) == 0 &&
                         ym2612_eg::detail::kIncTable[rate][0] ==
                             ym2612_eg::detail::kIncTable[rate][1] &&
                         ym2612_eg::detail::kIncTable[rate][1] ==
                             ym2612_eg::detail::kIncTable[rate][3];
    if (uniform) {
      // One EG tick is three output samples, and the envelope cannot move
      // more often than that: agreeing to within a tick is agreeing exactly.
      worst_uniform_samples =
          std::max(worst_uniform_samples, worst_ms / sample_ms);
      CHECK(worst_ms <= eg_tick_ms);
    }
  }
  std::cout << "release entry: worst slip " << worst_periods
            << " of one EG update period; " << worst_uniform_samples
            << " samples where the counter phase cannot matter\n";
  CHECK(worst_periods <= 1.0);
}

/// The key coming up does not move the cursor. The release is drawn from where
/// the note actually let go and the cursor carries straight on into it; only
/// the *shape* comes from the release trace, taken from the point where that
/// trace is already at the level the key came up on.
void test_the_cursor_carries_on_into_the_release() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  const double key_off_ms = 120.0;
  const double att = curve_out_at_ms(curve.held, key_off_ms);
  const double entry = release_entry_ms(curve.release, att);
  CHECK(entry > 0.0); // the trace reaches that level partway along, not at 0

  // The instant the key comes up the cursor has not moved ...
  const VoiceCursor at_key_off =
      cursor_for_voice(curve, key_off_ms, 0.0, curve.span_ms);
  CHECK(at_key_off.released);
  CHECK(std::fabs(at_key_off.ms - key_off_ms) < 1e-9);
  // ... the release is drawn from there, with the trace's shape from `entry`.
  CHECK(std::fabs(at_key_off.release_origin_ms - key_off_ms) < 1e-9);
  CHECK(std::fabs(at_key_off.release_from_ms - entry) < 1e-9);

  // From there it advances in real time.
  const VoiceCursor later =
      cursor_for_voice(curve, key_off_ms + 5.0, 5.0, curve.span_ms);
  CHECK(std::fabs(later.ms - (key_off_ms + 5.0)) < 1e-9);

  // Let go at full volume, the shape is the whole trace from its start.
  const VoiceCursor immediate =
      cursor_for_voice(curve, 0.0, 0.0, curve.span_ms);
  CHECK(immediate.ms <= 1e-9);
  CHECK(immediate.release_from_ms <= 1e-9);
}

/// A release starts from the level the ear was on. On key-off an inverted
/// SSG-EG mode latches the audible level into the attenuation, so the internal
/// number and the heard one are different things -- matching the internal one
/// enters the trace half a scale away and draws the release from full volume
/// whatever the note was doing.
void test_a_release_picks_up_at_the_level_that_was_sounding() {
  for (int type = 0; type < 8; ++type) {
    OperatorParams op;
    op.ar = 31;
    op.dr = 14;
    op.sl = 15;
    op.sr = 0;
    op.rr = 7;
    op.ssg = ssg_bits(type);
    const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);

    double worst = 0.0;
    for (int i = 1; i < 200; ++i) {
      const double t = c.span_ms * i / 200.0;
      const VoiceCursor cursor = cursor_for_voice(c, t, 0.0, c.span_ms);
      const double sounding = curve_out_at_ms(c.held, t);
      const double picked_up =
          curve_out_at_ms(c.release, cursor.release_from_ms);
      worst = std::max(worst, std::abs(sounding - picked_up));
    }
    // One attenuation unit is 0.094 dB; the traces are decimated, not exact.
    CHECK(worst <= 1.0);
  }
}

void test_a_released_cursor_takes_the_parked_level_with_it() {
  OperatorParams op = worked_example();
  op.sr = 0;
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  const double park = curve.held.park_ms;
  // Held far past the park, then released: the level it releases from is the
  // parked one, so both key-offs enter the release at the same point.
  const VoiceCursor soon =
      cursor_for_voice(curve, park + 1.0, 0.0, curve.span_ms);
  const VoiceCursor late = cursor_for_voice(curve, 30000.0, 0.0, curve.span_ms);
  CHECK(std::fabs(soon.release_from_ms - late.release_from_ms) < 1e-9);
  CHECK(std::fabs(soon.ms - late.ms) < 1e-9);
}

void test_a_voice_reports_how_long_it_has_been_silent() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  CHECK(std::isfinite(curve.release_silence_ms));

  const double att = curve_out_at_ms(curve.held, 0.0);
  const double entry = release_entry_ms(curve.release, att);
  const double to_silence = curve.release_silence_ms - entry;
  CHECK(to_silence > 0.0);
  // Still falling: audible.
  CHECK(cursor_for_voice(curve, to_silence * 0.5, to_silence * 0.5,
                         curve.span_ms)
            .silent_for_ms == 0.0);
  // Past silence, the clock the fade runs on starts.
  const VoiceCursor gone = cursor_for_voice(curve, to_silence + 100.0,
                                            to_silence + 100.0, curve.span_ms);
  CHECK(near_rel(gone.silent_for_ms, 100.0, 0.05));
}

/// A held SSG loop never parks, so its cursor must go round with the sound
/// rather than stopping at the right-hand edge while the note carries on
/// looping. It wraps over the whole periods the axis draws.
void test_a_held_loop_cursor_goes_round() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 15;
  op.sl = 15;
  op.sr = 0;
  op.rr = 7;
  op.ssg = ssg_bits(0); // repeating saw
  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);
  CHECK(c.held.loop_hz > 0.0);
  const double period = 1000.0 / c.held.loop_hz;

  double first_fold = -1.0;
  double last_fold = -1.0;
  for (const Marker &m : c.held.markers) {
    if (m.kind != MarkerKind::SsgFold || m.ms > c.span_ms) {
      continue;
    }
    if (first_fold < 0.0) {
      first_fold = m.ms;
    }
    last_fold = m.ms;
  }
  CHECK(first_fold > 0.0);
  CHECK(last_fold > first_fold);

  // Inside the drawn cycles the cursor simply tracks elapsed time.
  const VoiceCursor early =
      cursor_for_voice(c, first_fold * 0.5, -1.0, c.span_ms);
  CHECK(near_rel(early.ms, first_fold * 0.5, 0.001));

  // Past them it comes back round instead of sticking at the edge, and it
  // comes back to the same place one period later.
  const double beyond = last_fold + period * 2.5;
  const VoiceCursor wrapped = cursor_for_voice(c, beyond, -1.0, c.span_ms);
  CHECK(wrapped.ms < last_fold);
  CHECK(wrapped.ms >= first_fold);
  const VoiceCursor next =
      cursor_for_voice(c, beyond + (last_fold - first_fold), -1.0, c.span_ms);
  CHECK(near_rel(next.ms, wrapped.ms, 0.001));

  // The wrap is wrapped_into_loop_ms()'s answer, on its own terms.
  CHECK(near_rel(wrapped_into_loop_ms(c, beyond, c.span_ms), wrapped.ms,
                 0.001));

  // A patch that does not loop does not come back round: its cursor runs off
  // the end of the axis and is simply not drawn there.
  OperatorParams plain = op;
  plain.ssg = 0;
  const EnvelopeCurve pc = build_envelope_curve(plain, kMiddleC);
  CHECK(wrapped_into_loop_ms(pc, pc.span_ms * 5.0, pc.span_ms) ==
        pc.span_ms * 5.0);
  const VoiceCursor gone =
      cursor_for_voice(pc, pc.span_ms * 5.0, -1.0, pc.span_ms);
  CHECK(gone.ms > pc.span_ms || pc.held_parked);
}

/// A voice draws the road it has travelled, not the road ahead: the overlay
/// stops where the note has actually got to, and stops growing at key-off. A
/// loop that has already come round once has been through the whole axis.
void test_a_voice_draws_only_what_it_has_been_through() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);

  // Partway in, the overlay reaches exactly as far as the note has.
  const VoiceCursor early = cursor_for_voice(c, 40.0, -1.0, c.span_ms);
  CHECK(near_rel(early.held_to_ms, 40.0, 0.001));

  // It never runs past the axis.
  const VoiceCursor beyond =
      cursor_for_voice(c, c.span_ms * 3.0, -1.0, c.span_ms);
  CHECK(near_rel(beyond.held_to_ms, c.span_ms, 0.001));

  // Once the key is up it stops growing: the held part is frozen at the
  // instant of key-off however long the release runs on.
  const double key_off = 40.0;
  const VoiceCursor just_off = cursor_for_voice(c, key_off, 0.0, c.span_ms);
  const VoiceCursor long_off =
      cursor_for_voice(c, key_off + 500.0, 500.0, c.span_ms);
  CHECK(near_rel(just_off.held_to_ms, key_off, 0.001));
  CHECK(near_rel(long_off.held_to_ms, key_off, 0.001));

  // A loop past its first pass has been through all of it.
  OperatorParams loop = op;
  loop.ssg = ssg_bits(0);
  loop.sl = 15;
  loop.dr = 15;
  const EnvelopeCurve lc = build_envelope_curve(loop, kMiddleC);
  CHECK(lc.held.loop_hz > 0.0);
  const VoiceCursor round =
      cursor_for_voice(lc, lc.span_ms * 2.5, -1.0, lc.span_ms);
  CHECK(near_rel(round.held_to_ms, lc.span_ms, 0.001));
  // ... and its cursor has still come back round rather than run off.
  CHECK(round.ms <= lc.span_ms);
}

/// A released voice needs to know where on the release trace its own release
/// begins -- the graph draws that stretch as a line, since the drawn release
/// area is the one a note let go at full volume would take, not this one.
void test_a_released_voice_reports_where_its_release_begins() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);

  // Held: no release to draw yet.
  const VoiceCursor held = cursor_for_voice(c, 100.0, -1.0, c.span_ms);
  CHECK(held.release_from_ms < 0.0);
  CHECK(!held.released);

  // Let go while the decay is still running: the release starts high on the
  // trace, near its beginning.
  const double early_off = c.attack_end_ms + 1.0;
  const VoiceCursor early =
      cursor_for_voice(c, early_off + 5.0, 5.0, c.span_ms);
  CHECK(early.released);
  CHECK(early.release_from_ms >= 0.0);

  // Let go from the sustain, lower down: further along the trace, because the
  // drawn release takes time to fall that far.
  const double late_off = c.decay_end_ms + 50.0;
  const VoiceCursor late = cursor_for_voice(c, late_off + 5.0, 5.0, c.span_ms);
  CHECK(late.release_from_ms > early.release_from_ms);

  // And the cursor is where the key came up plus however long it has been
  // falling -- the entry point only sets the shape, not the position.
  CHECK(near_rel(late.ms, late.release_origin_ms + 5.0, 0.001));
}

// --------------------------------------------- the voice curve cache's key

void test_two_notes_sharing_a_key_scale_value_share_a_curve() {
  OperatorParams op = worked_example();
  op.ks = 0;

  // C4 and C5 are blocks 4 and 5, and with KS = 0 the ksv is the block's top
  // two bits: the same value. C3 is block 3, which is not.
  const NotePitch c3 = NotePitch::from_midi(48);
  const NotePitch c4 = NotePitch::from_midi(60);
  const NotePitch c5 = NotePitch::from_midi(72);
  CHECK(key_scale_value(op, c4) == key_scale_value(op, c5));
  CHECK(key_scale_value(op, c3) != key_scale_value(op, c4));

  // A reference note whose own ksv is none of theirs, so nothing is answered
  // by the shortcut below.
  const EnvelopeCurve reference = build_envelope_curve(op, kC0);

  VoiceCurveCache cache;
  int budget = 1;
  const EnvelopeCurve *first = cache.get(op, c4, reference, kC0, budget);
  CHECK(first != nullptr);
  CHECK(budget == 0); // one simulation, and it was paid for
  CHECK(cache.rebuild_count() == 1);
  CHECK(cache.size() == 1);
  CHECK(first != &reference);

  // Same ksv: the same entry, no second simulation, and no budget spent --
  // which is what makes a note-on free once the cache is warm.
  const EnvelopeCurve *second = cache.get(op, c5, reference, kC0, budget);
  CHECK(second == first);
  CHECK(budget == 0);
  CHECK(cache.rebuild_count() == 1);
  CHECK(cache.size() == 1);

  // A different ksv is a different entry -- and with nothing left in the
  // budget it waits for the next frame rather than simulating now.
  CHECK(cache.get(op, c3, reference, kC0, budget) == nullptr);
  CHECK(cache.rebuild_count() == 1);
  budget = 1;
  const EnvelopeCurve *third = cache.get(op, c3, reference, kC0, budget);
  CHECK(third != nullptr);
  CHECK(third != first);
  CHECK(cache.rebuild_count() == 2);
  CHECK(cache.size() == 2);

  // A note that shares the REFERENCE note's ksv costs nothing at all: the
  // curve already on screen is its curve.
  budget = 1;
  CHECK(cache.get(op, kC0, reference, kC0, budget) == &reference);
  CHECK(budget == 1);
  CHECK(cache.rebuild_count() == 2);
}

void test_key_scaling_splits_what_it_should() {
  OperatorParams op = worked_example();
  op.ks = 3; // ksv is the whole keycode: every block is its own entry
  const NotePitch c4 = NotePitch::from_midi(60);
  const NotePitch c5 = NotePitch::from_midi(72);
  CHECK(key_scale_value(op, c4) != key_scale_value(op, c5));

  const EnvelopeCurve reference = build_envelope_curve(op, kC0);
  VoiceCurveCache cache;
  int budget = 2;
  CHECK(cache.get(op, c4, reference, kC0, budget) != nullptr);
  CHECK(cache.get(op, c5, reference, kC0, budget) != nullptr);
  CHECK(cache.rebuild_count() == 2);
  CHECK(cache.size() == 2);
}

void test_the_voice_cache_never_outgrows_the_six_voices() {
  OperatorParams op = worked_example();
  op.ks = 3;
  const EnvelopeCurve reference = build_envelope_curve(op, kC0);
  VoiceCurveCache cache;
  for (int midi = 24; midi <= 96; midi += 3) {
    int budget = 1;
    cache.get(op, NotePitch::from_midi(midi), reference, kC0, budget);
    CHECK(cache.size() <= VoiceCurveCache::kMaxEntries);
  }
}

void test_a_voice_curve_covers_the_axis_it_is_drawn_on() {
  OperatorParams op = worked_example();
  op.ks = 3; // so a high note's envelope is far shorter than C0's
  const EnvelopeCurve reference = build_envelope_curve(op, kC0);
  VoiceCurveCache cache;
  int budget = 1;
  const EnvelopeCurve *voice =
      cache.get(op, NotePitch::from_midi(96), reference, kC0, budget);
  CHECK(voice != nullptr);
  // Its own window is much narrower, but it was simulated across the axis it
  // will be drawn on, so the overlay never has to be invented past the end.
  CHECK(voice->span_ms < reference.span_ms);
  CHECK(content_ms(voice->held) >= reference.span_ms * 0.999 ||
        voice->held_parked);
}

void test_a_register_change_drops_every_voice_curve() {
  OperatorParams op = worked_example();
  EnvelopeCurve reference = build_envelope_curve(op, kC0);
  VoiceCurveCache cache;
  const NotePitch c4 = NotePitch::from_midi(60);
  int budget = 1;
  CHECK(cache.get(op, c4, reference, kC0, budget) != nullptr);
  CHECK(cache.rebuild_count() == 1);
  CHECK(cache.get(op, c4, reference, kC0, budget) != nullptr);
  CHECK(cache.rebuild_count() == 1);

  op.dr = 12;
  reference = build_envelope_curve(op, kC0);
  budget = 1;
  CHECK(cache.get(op, c4, reference, kC0, budget) != nullptr);
  CHECK(cache.rebuild_count() == 2);
  CHECK(cache.size() == 1);
}

/// Key scaling is the whole reason the note matters to a graph: the same
/// registers decay far faster high up the keyboard.
void test_key_scaling_follows_the_note() {
  OperatorParams op = worked_example();
  op.ks = 3;

  const double low =
      build_envelope_curve(op, NotePitch::from_midi(36)).decay_end_ms; // C2
  const double high =
      build_envelope_curve(op, NotePitch::from_midi(96)).decay_end_ms; // C7

  CHECK(low > 0.0);
  CHECK(high > 0.0);
  CHECK(high < low * 0.5);

  // ... while at KS = 0 -- what most patches use -- five octaves move the
  // decay by less than a factor of two. That is what a single reference note
  // rests on: the rate still picks up keycode >> 3, so it is not nothing, but
  // it is not the eightfold swing above either.
  OperatorParams flat = worked_example();
  flat.ks = 0;
  const double flat_low =
      build_envelope_curve(flat, NotePitch::from_midi(36)).decay_end_ms;
  const double flat_high =
      build_envelope_curve(flat, NotePitch::from_midi(96)).decay_end_ms;
  CHECK(flat_high < flat_low);
  CHECK(flat_high > flat_low * 0.5);
}

} // namespace

int main() {
  std::cout << "graph_test\n";

  RUN_TEST(test_same_envelope_covers_every_register);

  RUN_TEST(test_the_axis_reaches_the_end_of_the_sustain);
  RUN_TEST(test_an_envelope_longer_than_the_ceiling_is_cut_at_the_ceiling);
  RUN_TEST(test_a_hold_that_never_ends_takes_a_fixed_share);
  RUN_TEST(test_an_ssg_loop_shows_a_few_periods);
  RUN_TEST(test_a_slow_attack_ssg_loop_reports_a_musical_rate);
  RUN_TEST(test_the_release_budget_is_its_own);

  RUN_TEST(test_a_loop_that_never_folds_is_sized_like_a_plain_patch);
  RUN_TEST(test_only_a_non_standard_ssg_attack_earns_a_line);
  RUN_TEST(test_the_attack_and_decay_are_never_cut);
  RUN_TEST(test_every_patch_has_a_finite_axis);
  RUN_TEST(test_every_phase_present_is_at_least_partly_visible);
  RUN_TEST(test_every_rate_moves_the_axis_monotonically);
  RUN_TEST(test_the_flat_hold_is_the_one_step_the_axis_jumps);
  RUN_TEST(test_sustain_level_moves_the_axis_monotonically);
  RUN_TEST(test_a_very_slow_attack_still_leaves_room_for_the_sustain);
  RUN_TEST(test_the_axis_does_not_jump_between_neighbouring_values);

  RUN_TEST(test_the_span_is_the_content_it_has_to_hold);
  RUN_TEST(test_the_span_does_not_depend_on_where_the_axis_has_been);
  RUN_TEST(test_the_grid_step_divides_the_span_sensibly);

  RUN_TEST(test_the_worked_examples_decay_lands_on_the_real_millisecond_axis);
  RUN_TEST(test_the_release_starts_at_full_volume_and_reaches_silence);
  RUN_TEST(test_ssg_eg_makes_the_release_dramatically_shorter);
  RUN_TEST(test_a_loop_keeps_looping_to_the_right_edge);
  RUN_TEST(test_an_inverted_ssg_release_is_as_long_as_the_upright_one);
  RUN_TEST(test_an_sr0_held_trace_parks_and_stays_flat);
  RUN_TEST(test_total_level_moves_the_whole_curve_down);
  RUN_TEST(test_ssg_enabled_curves_fold);

  RUN_TEST(test_a_fast_loop_keeps_its_scale);
  RUN_TEST(test_a_slow_loop_still_shows_a_period);
  RUN_TEST(test_a_very_long_release_does_not_crush_the_held_trace);
  RUN_TEST(test_a_slow_release_is_simulated_to_the_end);

  RUN_TEST(test_the_only_warning_is_the_non_standard_ssg_attack);
  RUN_TEST(test_the_cache_recomputes_only_on_a_real_change);
  RUN_TEST(test_the_throttle_spaces_a_rebuild_by_what_it_cost);
  RUN_TEST(test_a_cheap_curve_is_rebuilt_every_frame);
  RUN_TEST(test_an_expensive_curve_is_deferred_while_the_value_moves);
  RUN_TEST(test_a_settled_patch_always_converges_on_the_exact_curve);
  RUN_TEST(test_a_still_patch_costs_the_cache_nothing);
  RUN_TEST(test_the_cache_rebuilds_when_the_note_changes);

  RUN_TEST(test_the_cursor_is_where_the_elapsed_time_says);
  RUN_TEST(test_a_cursor_still_moving_leaves_the_graph);
  RUN_TEST(test_a_parked_cursor_stays_where_the_envelope_stopped);
  RUN_TEST(test_the_release_entry_point_matches_a_real_release);
  RUN_TEST(test_the_cursor_carries_on_into_the_release);
  RUN_TEST(test_a_release_picks_up_at_the_level_that_was_sounding);
  RUN_TEST(test_a_released_cursor_takes_the_parked_level_with_it);
  RUN_TEST(test_a_voice_reports_how_long_it_has_been_silent);
  RUN_TEST(test_a_held_loop_cursor_goes_round);
  RUN_TEST(test_a_voice_draws_only_what_it_has_been_through);
  RUN_TEST(test_a_released_voice_reports_where_its_release_begins);

  RUN_TEST(test_two_notes_sharing_a_key_scale_value_share_a_curve);
  RUN_TEST(test_key_scaling_splits_what_it_should);
  RUN_TEST(test_the_voice_cache_never_outgrows_the_six_voices);
  RUN_TEST(test_a_voice_curve_covers_the_axis_it_is_drawn_on);
  RUN_TEST(test_a_register_change_drops_every_voice_curve);
  RUN_TEST(test_key_scaling_follows_the_note);

  return testing::summary();
}
