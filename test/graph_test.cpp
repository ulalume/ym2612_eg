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

/// A clock the test drives, frame by frame: hands out `now_ms`, then steps
/// itself on by `cost_ms`, so a rebuild's two clock reads are exactly
/// `cost_ms` apart.
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

/// A clock that leaps a whole second every read, past any interval the
/// throttle could ask for, so it never defers a rebuild.
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

/// Pins that same_envelope() compares all eight registers.
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

/// Pins that a sustain which finishes is drawn to its exact end, not a
/// compressed or approximated version of it, over a sweep of registers.
void test_the_axis_reaches_the_end_of_the_sustain() {
  OperatorParams op = adsr(8, 28, 4, 15, 11, 0);
  op.tl = 7;
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(timeline.lifetime_ms() < kMaxSpanMs);
  CHECK(choose_held_ms(op, kMiddleC) == timeline.lifetime_ms());

  const CurveResult held = simulate_held(op, kMiddleC);
  const double decay_end = first_marker_ms(held, MarkerKind::DecayEnd);
  CHECK(decay_end > 0.0);
  CHECK(near_rel(decay_end, timeline.sustain_start_ms(), 0.02));
  CHECK(decay_end < choose_held_ms(op, kMiddleC));
  CHECK(std::isfinite(held.park_ms));
  CHECK(near_rel(held.park_ms, choose_held_ms(op, kMiddleC), 0.02));

  for (int ar = 0; ar <= 31; ar += 3) {
    for (int dr = 1; dr <= 31; dr += 3) {
      for (int sl = 0; sl <= 15; sl += 3) {
        for (int sr = 1; sr <= 31; sr += 3) {
          const OperatorParams swept = adsr(ar, dr, sl, sr, 7, 0);
          const PhaseDurations phases = phase_durations(swept, kMiddleC);
          if (phases.lifetime_ms() >= kMinHeldMs &&
              phases.lifetime_ms() <= kMaxSpanMs) {
            CHECK(choose_held_ms(swept, kMiddleC) == phases.lifetime_ms());
          }
        }
      }
    }
  }
}

/// Pins that an envelope longer than kMaxSpanMs is cut at the ceiling,
/// taking the sustain's tail and never the attack/decay shape before it.
void test_an_envelope_longer_than_the_ceiling_is_cut_at_the_ceiling() {
  const OperatorParams op = worked_example();
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(timeline.lifetime_ms() > 20000.0);
  CHECK(timeline.lifetime_ms() < 35000.0);
  CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);

  CHECK(timeline.sustain_start_ms() < kMaxSpanMs);
  CHECK(first_marker_ms(simulate_held(op, kMiddleC), MarkerKind::DecayEnd) <
        kMaxHeldMs);
}

/// Pins that SR = 0 (a hold that never ends) takes a fixed kFlatHoldShare of
/// the graph, whatever the attack and decay are, and that SR = 1 does not.
void test_a_hold_that_never_ends_takes_a_fixed_share() {
  OperatorParams op = worked_example();
  op.sr = 0;

  const CurveResult held = simulate_held(op, kMiddleC);
  CHECK(std::isfinite(held.park_ms));
  const PhaseDurations timeline = phase_durations(op, kMiddleC);
  CHECK(!std::isfinite(timeline.sustain_ms));
  CHECK(!std::isfinite(timeline.lifetime_ms()));

  const double held_ms = choose_held_ms(op, kMiddleC);
  CHECK(near_rel(timeline.sustain_start_ms() / held_ms, 1.0 - kFlatHoldShare,
                 1e-12));
  CHECK(near_rel(held.park_ms, timeline.sustain_start_ms(), 0.02));
  CHECK(held.park_ms < held_ms);

  for (int ar = 1; ar <= 31; ar += 2) {
    for (int dr = 2; dr <= 31; dr += 3) {
      const OperatorParams swept = adsr(ar, dr, 4, 0, 7, 0);
      const double start =
          phase_durations(swept, kMiddleC).sustain_start_ms();
      const double asked = start / (1.0 - kFlatHoldShare);
      if (asked >= kMinHeldMs && asked <= kMaxSpanMs) {
        CHECK(near_rel(start / choose_held_ms(swept, kMiddleC),
                       1.0 - kFlatHoldShare, 1e-12));
      }
    }
  }

  op.sr = 1;
  CHECK(!std::isfinite(timeline.sustain_ms));
  CHECK(std::isfinite(phase_durations(op, kMiddleC).sustain_ms));
  CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);
  CHECK(choose_held_ms(op, kMiddleC) > held_ms * 10.0);

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

/// Pins that an SSG loop's reported rate is musical (Hz), not the sample rate
/// a naive per-sample fold count would give for a slow attack.
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
  CHECK(held.loop_hz > 1.0);
  CHECK(held.loop_hz < 100.0);
  CHECK(near_rel(held.loop_hz, 6.12, 0.02));

  const double held_ms = choose_held_ms(op, kMiddleC);
  const double periods = held_ms / (1000.0 / held.loop_hz);
  CHECK(periods >= 3.0);
  CHECK(periods <= 4.0);
}

/// Pins that release_max_ms() does not depend on the held envelope's own
/// registers.
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

/// Pins that a loop whose ramp never finishes (infinite period) or that
/// latches instead of folding (zero period) sizes its axis like a plain
/// patch, via window_for_timeline_ms().
void test_a_loop_that_never_folds_is_sized_like_a_plain_patch() {
  const OperatorParams stalled = ssg_patch(0, 31, 15, 8, 0, 7, 0); // SR = 0
  CHECK(!std::isfinite(ssg_loop_period_ms(stalled, kMiddleC)));
  CHECK(choose_held_ms(stalled, kMiddleC) ==
        window_for_timeline_ms(phase_durations(stalled, kMiddleC)));
  const OperatorParams latching = ssg_patch(1, 31, 15, 8, 8, 7, 0);
  CHECK(ssg_loop_period_ms(latching, kMiddleC) == 0.0);
  CHECK(choose_held_ms(latching, kMiddleC) ==
        window_for_timeline_ms(phase_durations(latching, kMiddleC)));
}

/// Pins that only an SSG-EG patch with AR < 31 earns the warning line, in
/// every SSG-EG mode.
void test_only_a_non_standard_ssg_attack_earns_a_line() {
  const auto warning_for = [](const OperatorParams &op) {
    return build_envelope_curve(op, kMiddleC).warning;
  };
  CHECK(warning_for(ssg_patch(0, 30, 15, 4, 8, 7, 0)) != nullptr);
  CHECK(warning_for(ssg_patch(0, 31, 15, 4, 8, 7, 0)) == nullptr);
  CHECK(warning_for(adsr(30, 15, 4, 8, 7, 0)) == nullptr);
  for (int type = 0; type < 8; ++type) {
    CHECK(warning_for(ssg_patch(type, 0, 15, 4, 8, 7, 0)) != nullptr);
    CHECK(warning_for(ssg_patch(type, 31, 15, 4, 8, 7, 0)) == nullptr);
  }
}

/// Pins that the attack and decay are never cut by the width ceiling, even
/// when the sustain or the attack/decay themselves outlast it (AR = 1 alone
/// runs 8.4 s).
void test_the_attack_and_decay_are_never_cut() {
  PhaseDurations timeline;
  timeline.attack_ms = 8360.0;
  timeline.decay_ms = 460.0;
  timeline.sustain_ms = 200.0;
  CHECK(window_for_timeline_ms(timeline) == timeline.lifetime_ms());

  PhaseDurations spilling = timeline;
  spilling.sustain_ms = 60000.0;
  CHECK(window_for_timeline_ms(spilling) > spilling.sustain_start_ms());
  CHECK(window_for_timeline_ms(spilling) == kMaxSpanMs);

  PhaseDurations enormous;
  enormous.attack_ms = 12000.0;
  enormous.decay_ms = 3000.0;
  enormous.sustain_ms = 500.0;
  CHECK(enormous.sustain_start_ms() < kMaxSpanMs);
  CHECK(window_for_timeline_ms(enormous) == enormous.lifetime_ms());

  PhaseDurations beyond = enormous;
  beyond.attack_ms = 30000.0;
  CHECK(beyond.sustain_start_ms() > kMaxSpanMs);
  CHECK(window_for_timeline_ms(beyond) == kMaxSpanMs);
  CHECK(window_for_timeline_ms(enormous) > kMaxHeldMs);

  const OperatorParams slow = adsr(1, 12, 6, 4, 5, 0);
  const PhaseDurations real = phase_durations(slow, kMiddleC);
  CHECK(near_rel(real.attack_ms, 8360.0, 0.02));
  CHECK(choose_held_ms(slow, kMiddleC) > real.sustain_start_ms());
}

/// Pins that a phase which never advances (AR = 0 or DR = 0, an infinite
/// sum) still comes back with a finite, bounded axis.
void test_every_patch_has_a_finite_axis() {
  PhaseDurations frozen_attack;
  frozen_attack.attack_ms = std::numeric_limits<double>::infinity();
  frozen_attack.decay_ms = 100.0;
  frozen_attack.sustain_ms = 100.0;
  CHECK(window_for_timeline_ms(frozen_attack) == kMaxSpanMs);

  for (const OperatorParams &op :
       {adsr(0, 10, 4, 5, 7, 0), adsr(31, 0, 4, 5, 7, 0)}) {
    CHECK(!std::isfinite(phase_durations(op, kMiddleC).sustain_start_ms()));
    CHECK(choose_held_ms(op, kMiddleC) == kMaxSpanMs);
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
    CHECK(std::isfinite(curve.span_ms));
    CHECK(curve.span_ms <= kMaxSpanMs);
  }

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

  const OperatorParams instant = adsr(31, 0, 0, 0, 7, 0);
  CHECK(choose_held_ms(instant, kMiddleC) == kMinHeldMs);
  const EnvelopeCurve curve = build_envelope_curve(instant, kMiddleC);
  CHECK(curve.held_ms == kMinHeldMs);
  CHECK(curve.span_ms >= kMinSpanMs);
}

/// Pins that the instant the sustain begins is always inside the axis, so
/// every phase the envelope actually has is at least partly drawn.
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

/// Two spans that are the same width: summing the same lifetime in a
/// different order (as SL moves attenuation between phases) can differ in the
/// last bits of a double, far finer than a pixel.
constexpr double kSameSpan = 1e-9;

bool moves(const std::vector<double> &spans) {
  for (const double span : spans) {
    if (!near_rel(span, spans.front(), kSameSpan)) {
      return true;
    }
  }
  return false;
}

/// The base patches the sweeps run over.
const OperatorParams &sweep_base(int i) {
  static const OperatorParams bases[] = {
      slow_patch(),               worked_example(),
      adsr(31, 20, 8, 20, 12, 0), adsr(20, 15, 6, 10, 9, 3),
      adsr(6, 4, 10, 3, 2, 0),    adsr(31, 8, 2, 6, 10, 0),
  };
  return bases[i];
}
constexpr int kSweepBases = 6;

/// Pins that every rate register moves the axis one way only (faster never
/// widens, slower never narrows), continuously, and that it moves at all.
/// Register 0 (a phase that never advances) is exempt: the step off it is a
/// deliberate discontinuity, not a bug.
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
        CHECK(spans[v] >= spans[v - 1] * 0.49);
      }
      moved_somewhere |= moves(spans);
    }
    CHECK(moved_somewhere);
  }
}

/// Pins that SR = 0 -> SR = 1 is the one step the axis is allowed to jump:
/// the flat-hold share gives way to the real (much longer) sustain length.
void test_the_flat_hold_is_the_one_step_the_axis_jumps() {
  for (int i = 0; i < kSweepBases; ++i) {
    const std::vector<double> spans = span_sweep(sweep_base(i), Field::Sr);
    CHECK(spans[1] >= spans[0] || near_rel(spans[1], spans[0], kSameSpan));
  }
  OperatorParams op = worked_example();
  op.sr = 0;
  const double flat = choose_held_ms(op, kMiddleC);
  op.sr = 1;
  CHECK(choose_held_ms(op, kMiddleC) > flat * 10.0);
}

/// Pins that SL moves the axis monotonically -- one direction for the whole
/// of a given patch's sweep, though which direction is a property of the
/// patch (raising SL can lengthen or shorten the axis).
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
      CHECK(!(risen && fallen));
    }
    moved_somewhere |= moves(spans);
  }
  CHECK(moved_somewhere);

  const std::vector<double> slow_decay =
      span_sweep(adsr(31, 4, 0, 20, 12, 0), Field::Sl);
  CHECK(slow_decay.back() > slow_decay.front());
  const std::vector<double> quick_decay =
      span_sweep(adsr(31, 20, 0, 4, 12, 0), Field::Sl);
  CHECK(quick_decay.back() < quick_decay.front());
}

/// Pins that a very slow attack (AR = 1, 8.4 s) still leaves the axis reaching
/// past the sustain's start, for every SR, with the attack and decay ending
/// inside the graph.
void test_a_very_slow_attack_still_leaves_room_for_the_sustain() {
  double previous = 0.0;
  for (int sr = 1; sr <= 31; ++sr) {
    OperatorParams op = adsr(1, 12, 6, sr, 5, 0);
    op.tl = 0;
    const PhaseDurations timeline = phase_durations(op, kMiddleC);
    const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);

    CHECK(near_rel(timeline.attack_ms, 8360.0, 0.02));
    CHECK(timeline.sustain_start_ms() > 3000.0);
    CHECK(curve.span_ms > timeline.sustain_start_ms());
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

/// Pins that no neighbouring pair of sustain levels moves the axis more than
/// a factor of two, except SL = 15 -- a discontinuity of the chip itself
/// (sustain_attenuation() jumps straight to 0x3E0 there).
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
      CHECK(span <= previous * 2.01);
      CHECK(span >= previous * 0.49);
      previous = span;
    }
    CHECK(sweep(15, sr) < previous);
  }
}

// ------------------------------------------------------------------ the axis

/// Pins that the span is simply the content it has to hold (held window and
/// release), at full precision -- not rounded onto a ladder of round numbers.
void test_the_span_is_the_content_it_has_to_hold() {
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
    CHECK(curve.span_ms >= kMinSpanMs);
  }
}

/// Pins that the curve is a pure function of the operator and the note: the
/// axis does not depend on where the cache has been.
void test_the_span_does_not_depend_on_where_the_axis_has_been() {
  EnvelopeCurveCache first;
  EnvelopeCurveCache second;
  first.set_clock(&leaping_clock);
  second.set_clock(&leaping_clock);
  const OperatorParams a = worked_example();
  const OperatorParams b = adsr(6, 4, 10, 3, 2, 0);

  first.get(a, kMiddleC);
  first.get(b, kMiddleC);
  const double after_a_detour = first.get(a, kMiddleC).span_ms;
  const double straight = second.get(a, kMiddleC).span_ms;
  CHECK(after_a_detour == straight);
  CHECK(build_envelope_curve(a, kMiddleC).span_ms == straight);
}

/// Pins that grid_step_ms() gives 1-6 divisions across every span the width
/// policy can hand it.
void test_the_grid_step_divides_the_span_sensibly() {
  for (double span = kMinSpanMs; span <= kLoopMaxAxisMs; span += 5.0) {
    const double step = grid_step_ms(span);
    CHECK(step > 0.0);
    CHECK(span / step <= 6.0 || step == 10000.0);
    CHECK(span / step >= 1.0);
  }
}

// ------------------------------------------------------ end-to-end geometry

/// Pins that build_envelope_curve()'s decay marker lands on EG_SPEC's worked
/// example (306.4 ms), and that everything else drawn shares that same axis.
void test_the_worked_examples_decay_lands_on_the_real_millisecond_axis() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  CHECK(curve.decay_end_ms > 0.0);
  CHECK(near_rel(curve.decay_end_ms, 306.4, 0.02));

  CHECK(curve.attack_end_ms >= 0.0);
  CHECK(curve.attack_end_ms < curve.decay_end_ms);
  CHECK(curve.held_ms > curve.decay_end_ms);
  CHECK(curve.span_ms >= curve.held_ms);
  CHECK(near_rel(content_ms(curve.held), curve.span_ms, 0.01));
  CHECK(!curve.held.points.empty());
  CHECK(curve.warning == nullptr);

  CHECK(!has_marker(curve.held, MarkerKind::KeyOff));
  CHECK(!curve.held_parked); // SR = 5 keeps crawling

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

/// Pins that the release trace starts at full volume, from t = 0, and reaches
/// silence.
void test_the_release_starts_at_full_volume_and_reaches_silence() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  CHECK(curve.release.points.size() >= 2);
  CHECK(curve.release.points.front().ms == 0.0f);
  CHECK(curve.release.points.front().out == curve.peak_out);
  CHECK(!curve.release_truncated);
  CHECK(curve.release.points.back().out == kMaxAttenuation);
  CHECK(near_rel(curve.release_content_ms, 907.7, 0.02));
  double previous_out = -1.0;
  for (const auto &p : curve.release.points) {
    CHECK(p.out >= previous_out);
    previous_out = p.out;
  }
}

/// Pins that SSG-EG's release (quadrupled increment, cut dead at 0x200) is
/// several times shorter than a plain release at the same RR.
void test_ssg_eg_makes_the_release_dramatically_shorter() {
  OperatorParams plain = worked_example();
  plain.rr = 6;
  OperatorParams ssg = plain;
  ssg.ssg = ssg_bits(0); // repeating saw

  const EnvelopeCurve plain_curve = build_envelope_curve(plain, kMiddleC);
  const EnvelopeCurve ssg_curve = build_envelope_curve(ssg, kMiddleC);

  CHECK(near_rel(plain_curve.release_content_ms, 1815.3, 0.02));
  CHECK(near_rel(ssg_curve.release_content_ms, 229.8, 0.02));
  CHECK(ssg_curve.release_content_ms * 4.0 < plain_curve.release_content_ms);
  CHECK(plain_curve.release.points.back().out == kMaxAttenuation);
  CHECK(ssg_curve.release.points.back().out == kMaxAttenuation);
}

/// Pins that the held trace is simulated across the whole span, so a loop
/// keeps looping to the right edge instead of being extrapolated.
void test_a_loop_keeps_looping_to_the_right_edge() {
  const OperatorParams patches[] = {
      ssg_patch(4, 14, 18, 9, 14, 0, 0),
      ssg_patch(4, 29, 1, 0, 7, 0, 0),
      ssg_patch(4, 8, 10, 7, 4, 0, 0),
  };
  for (const auto &op : patches) {
    const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);
    CHECK(c.held.loop_hz > 0.1);
    CHECK(c.held.loop_hz < 100.0);
    CHECK(near_rel(content_ms(c.held), c.span_ms, 0.01));
    CHECK(!c.held_parked);
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
    CHECK(!has_marker(c.held, MarkerKind::KeyOff));
    CHECK(c.release_content_ms > 100.0);
  }
}

/// Pins that an inverted SSG-EG release (attenuation scale run backwards)
/// starts from whatever level output() is loudest at and releases at exactly
/// the same speed as its upright twin.
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
  CHECK(inv.release_content_ms > 100.0);
  CHECK(inv.release.points.front().out == inv.peak_out + 1);
  CHECK(inv.release.points.back().out == kMaxAttenuation);
  CHECK(!inv.release_truncated);
  double previous_out = -1.0;
  for (const auto &p : inv.release.points) {
    CHECK(p.out >= previous_out);
    previous_out = p.out;
  }
}

/// Pins that SR = 0 parks the held trace at the sustain level the moment the
/// decay reaches it, and stays flat, with the flat stretch visibly wider than
/// a single pixel.
void test_an_sr0_held_trace_parks_and_stays_flat() {
  OperatorParams op = worked_example();
  op.sr = 0;
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);

  CHECK(curve.held_parked);
  CHECK(!has_marker(curve.held, MarkerKind::KeyOff));
  CHECK(std::isfinite(curve.held.park_ms));
  CHECK(near_rel(curve.held.park_ms, 306.4, 0.02));
  CHECK(curve.held.points.back().out == curve.sustain_out);
  for (const auto &p : curve.held.points) {
    CHECK(p.out <= curve.sustain_out);
    if (p.ms >= curve.held.park_ms) {
      CHECK(p.out == curve.sustain_out);
    }
  }
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

/// Pins that an audio-rate loop keeps its own scale rather than the axis
/// being set by its much longer release, and still gets a readable share.
void test_a_fast_loop_keeps_its_scale() {
  OperatorParams op;
  op.ar = 31;
  op.dr = 24;
  op.sl = 15;
  op.rr = 7;
  op.ssg = ssg_bits(0);

  const EnvelopeCurve fast = build_envelope_curve(op, kMiddleC);
  CHECK(fast.held.loop_hz > 100.0);
  CHECK(fast.release_content_ms > fast.span_ms);
  CHECK(fast.span_ms <= fast.held_ms * 3.0);
  CHECK(fast.held_ms / fast.span_ms > 0.25);

  OperatorParams slow = op;
  slow.dr = 15;
  const EnvelopeCurve wide = build_envelope_curve(slow, kMiddleC);
  CHECK(wide.held.loop_hz < 20.0);
  CHECK(wide.held_ms / wide.span_ms > 0.25);
}

/// Pins that a slower loop never gets a narrower axis, and that one whole
/// period stays on it -- unless no axis could hold one, in which case the
/// widest axis there is, with a ramp on it.
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
    if (period * kLoopVisiblePeriods <= kLoopMaxAxisMs) {
      CHECK(folds >= 2);
    } else {
      CHECK(near_rel(c.span_ms, kLoopMaxAxisMs, 0.001));
      CHECK(folds >= 1);
    }
    CHECK(c.span_ms >= std::min(period, kLoopMaxAxisMs));
    CHECK(c.span_ms >= previous * 0.999);
    previous = c.span_ms;
  }
}

/// Pins that a very long release (RR = 2) does not crush the held trace's
/// share of the axis, and that a release which fits is drawn whole.
void test_a_very_long_release_does_not_crush_the_held_trace() {
  OperatorParams op = worked_example();
  op.rr = 2;
  const EnvelopeCurve slow = build_envelope_curve(op, kMiddleC);
  CHECK(slow.release_truncated); // still falling when the budget ran out
  CHECK(slow.release.points.back().out < kMaxAttenuation);
  CHECK(slow.held_ms / slow.span_ms > 0.15);

  OperatorParams fits = worked_example();
  fits.rr = 6;
  const EnvelopeCurve normal = build_envelope_curve(fits, kMiddleC);
  CHECK(!normal.release_truncated);
  CHECK(normal.span_ms >= normal.release_content_ms);
}

/// Pins that a slow release is simulated to the end rather than stopped
/// early, and that it (not the parked held trace) sets the axis width.
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
  CHECK(c.held_parked);
  CHECK(c.span_ms > c.held_ms);
  CHECK(c.span_ms >= c.held_ms * 3.9);
}

// -------------------------------------------------------------- warnings

/// Pins that the only warning the graph draws is a non-standard SSG-EG
/// attack (AR < 31); every other simulator defect is left to the curve shape.
void test_the_only_warning_is_the_non_standard_ssg_attack() {
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
  OperatorParams plain = slow_attack;
  plain.ssg = 0;
  CHECK(build_envelope_curve(plain, kMiddleC).warning == nullptr);

  OperatorParams frozen = worked_example();
  frozen.ar = 0;
  CHECK(build_envelope_curve(frozen, kMiddleC).warning == nullptr);

  OperatorParams never_loops;
  never_loops.ar = 31;
  never_loops.dr = 15;
  never_loops.sl = 4;
  never_loops.sr = 0;
  never_loops.rr = 7;
  never_loops.ssg = ssg_bits(0);
  const EnvelopeCurve stuck = build_envelope_curve(never_loops, kMiddleC);
  CHECK(stuck.warning == nullptr);

  OperatorParams audio_rate;
  audio_rate.ar = 31;
  audio_rate.dr = 24;
  audio_rate.sl = 15;
  audio_rate.rr = 7;
  audio_rate.ssg = ssg_bits(0);
  const EnvelopeCurve fast = build_envelope_curve(audio_rate, kMiddleC);
  CHECK(fast.held.loop_hz > 100.0);
  CHECK(fast.warning == nullptr);

  CHECK(build_envelope_curve(worked_example(), kMiddleC).warning == nullptr);
}

// ----------------------------------------------------------------- cache

/// Pins that EnvelopeCurveCache rebuilds only when the registers or note
/// actually change.
void test_the_cache_recomputes_only_on_a_real_change() {
  EnvelopeCurveCache cache;
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

/// Pins that RebuildThrottle::interval_for_ms() is proportional to cost, up
/// to kMaxRebuildDeferMs, and that may_rebuild()/note_rebuild() honor it.
void test_the_throttle_spaces_a_rebuild_by_what_it_cost() {
  CHECK(near_rel(RebuildThrottle::interval_for_ms(kRebuildBudgetMs),
                 kRebuildBudgetPeriodMs, 1e-9));
  CHECK(RebuildThrottle::interval_for_ms(kCheapBuildMs) < kFrameMs);
  CHECK(RebuildThrottle::interval_for_ms(kExpensiveBuildMs) > kFrameMs);

  double previous = 0.0;
  for (double cost = 0.0; cost < 20.0; cost += 0.05) {
    const double interval = RebuildThrottle::interval_for_ms(cost);
    CHECK(interval >= previous);
    CHECK(interval <= kMaxRebuildDeferMs);
    previous = interval;
  }
  CHECK(RebuildThrottle::interval_for_ms(1000.0) == kMaxRebuildDeferMs);

  RebuildThrottle throttle;
  CHECK(throttle.may_rebuild(0.0));

  throttle.note_rebuild(100.0, kExpensiveBuildMs);
  const double interval = RebuildThrottle::interval_for_ms(kExpensiveBuildMs);
  CHECK(throttle.interval_ms() == interval);
  CHECK(!throttle.may_rebuild(100.0));
  CHECK(!throttle.may_rebuild(100.0 + interval - 0.001));
  CHECK(throttle.may_rebuild(100.0 + interval + 0.001));

  throttle.note_rebuild(200.0, 0.0);
  CHECK(throttle.may_rebuild(200.0));
}

/// Pins that an ordinary (cheap) patch is rebuilt every frame, untouched by
/// the throttle.
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

/// Pins that an expensive rebuild is spaced across frames by the throttle,
/// with the curve already on hand drawn in between.
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
  CHECK(drawn_peak == 57 * 8);

  g_clock.now_ms = 60 * kFrameMs;
  CHECK(cache.get(op, kMiddleC).peak_out == 59 * 8);
  CHECK(cache.rebuild_count() == 21);
}

/// Pins that the value a drag ENDS on is always simulated for real, one
/// frame later, whichever frame it ends on -- and stays exact after.
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
    g_clock.now_ms = frames * kFrameMs;
    CHECK(same_drawn_curve(cache.get(op, kMiddleC),
                           build_envelope_curve(op, kMiddleC)));
    const int settled = cache.rebuild_count();
    cache.get(op, kMiddleC);
    CHECK(cache.rebuild_count() == settled);
  }
}

/// Pins that a still patch (unchanged frame to frame) costs the cache no
/// rebuild and not even a look at the clock.
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

/// Pins that the note is part of what a cached curve was built at, so moving
/// it invalidates the curve exactly as a register change does.
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
  request.op.ar = 0; // matches the drawn release: AR is irrelevant to it
  request.pitch = pitch;
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

/// Pins that a held, still-moving cursor is simply the elapsed time, clamped
/// at zero.
void test_the_cursor_is_where_the_elapsed_time_says() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  for (const double elapsed : {0.0, 1.0, 12.5, 100.0}) {
    const VoiceCursor cursor =
        cursor_for_voice(curve, elapsed, -1.0, curve.span_ms);
    CHECK(!cursor.released);
    CHECK(std::fabs(cursor.ms - elapsed) < 1e-9);
    CHECK(cursor.silent_for_ms == 0.0);
  }
  CHECK(cursor_for_voice(curve, -5.0, -1.0, curve.span_ms).ms == 0.0);
}

/// Pins that a cursor still moving past the axis leaves the graph (is not
/// pinned to the edge, which would say the envelope had come to rest there).
void test_a_cursor_still_moving_leaves_the_graph() {
  const EnvelopeCurve curve = build_envelope_curve(worked_example(), kMiddleC);
  CHECK(!curve.held_parked || curve.held.park_ms > curve.span_ms);
  const VoiceCursor cursor =
      cursor_for_voice(curve, curve.span_ms * 4.0, -1.0, curve.span_ms);
  CHECK(cursor.ms > curve.span_ms);
}

/// Pins that a parked cursor stays at the sustain level once the envelope
/// stops (SR = 0), however long the key is held, with nothing to fade out.
void test_a_parked_cursor_stays_where_the_envelope_stopped() {
  OperatorParams op = worked_example();
  op.sr = 0; // reaches the sustain level and holds
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  CHECK(curve.held_parked);
  const double park = curve.held.park_ms;
  CHECK(park < curve.span_ms); // the axis is wider, so this is a real clamp

  CHECK(std::fabs(cursor_for_voice(curve, park * 0.5, -1.0, curve.span_ms).ms -
                  park * 0.5) < 1e-9);
  for (const double elapsed : {park + 1.0, park * 2.0, 60000.0}) {
    const VoiceCursor cursor =
        cursor_for_voice(curve, elapsed, -1.0, curve.span_ms);
    CHECK(std::fabs(cursor.ms - park) < 1e-9);
  }
  CHECK(cursor_for_voice(curve, 60000.0, -1.0, curve.span_ms).silent_for_ms ==
        0.0);
}

/// Pins that a release starting from level L agrees with the drawn
/// release-from-full-volume, entered later -- to within one update period of
/// the release rate, the hardware's own counter-phase slip.
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
    const double update_ms =
        static_cast<double>(1 << ym2612_eg::detail::rate_shift(rate)) *
        eg_tick_ms;

    double worst_ms = 0.0;
    for (const double key_off_ms : {20.0, 40.0, 120.0, 400.0, 1200.0}) {
      const auto att =
          static_cast<uint16_t>(curve_out_at_ms(curve.held, key_off_ms) + 0.5);
      const double entry = release_entry_ms(curve.release, att);
      const CurveResult real = simulated_release_from(op, kMiddleC, att);
      if (real.points.size() < 2 || curve.release.points.size() < 2) {
        continue; // an instant cut has no trajectory to compare
      }
      const double ceiling = std::min<double>(curve.release.points.back().att,
                                              real.points.back().att);
      for (double level = att + 32.0; level <= ceiling; level += 64.0) {
        const double drawn = release_entry_ms(curve.release, level) - entry;
        const double actual = release_entry_ms(real, level);
        worst_ms = std::max(worst_ms, std::fabs(drawn - actual));
      }
    }
    CHECK(worst_ms <= update_ms);
    worst_periods = std::max(worst_periods, worst_ms / update_ms);

    const bool uniform = ym2612_eg::detail::rate_shift(rate) == 0 &&
                         ym2612_eg::detail::kIncTable[rate][0] ==
                             ym2612_eg::detail::kIncTable[rate][1] &&
                         ym2612_eg::detail::kIncTable[rate][1] ==
                             ym2612_eg::detail::kIncTable[rate][3];
    if (uniform) {
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

/// Pins that the key coming up does not move the cursor: the release is drawn
/// from where the note let go, carrying straight on, with only the *shape*
/// taken from the release trace at the level the key came up on.
void test_the_cursor_carries_on_into_the_release() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  const double key_off_ms = 120.0;
  const double att = curve_out_at_ms(curve.held, key_off_ms);
  const double entry = release_entry_ms(curve.release, att);
  CHECK(entry > 0.0); // the trace reaches that level partway along, not at 0

  const VoiceCursor at_key_off =
      cursor_for_voice(curve, key_off_ms, 0.0, curve.span_ms);
  CHECK(at_key_off.released);
  CHECK(std::fabs(at_key_off.ms - key_off_ms) < 1e-9);
  CHECK(std::fabs(at_key_off.release_origin_ms - key_off_ms) < 1e-9);
  CHECK(std::fabs(at_key_off.release_from_ms - entry) < 1e-9);

  const VoiceCursor later =
      cursor_for_voice(curve, key_off_ms + 5.0, 5.0, curve.span_ms);
  CHECK(std::fabs(later.ms - (key_off_ms + 5.0)) < 1e-9);

  const VoiceCursor immediate =
      cursor_for_voice(curve, 0.0, 0.0, curve.span_ms);
  CHECK(immediate.ms <= 1e-9);
  CHECK(immediate.release_from_ms <= 1e-9);
}

/// Pins that a release starts from the level the ear was on, in every SSG-EG
/// mode -- not the internal attenuation, which an inverted mode latches
/// differently on key-off.
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
    CHECK(worst <= 1.0); // one attenuation unit; the traces are decimated
  }
}

/// Pins that a cursor held far past the park, then released, releases from
/// the parked level: both a soon and a late key-off enter the release at the
/// same point.
void test_a_released_cursor_takes_the_parked_level_with_it() {
  OperatorParams op = worked_example();
  op.sr = 0;
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  const double park = curve.held.park_ms;
  const VoiceCursor soon =
      cursor_for_voice(curve, park + 1.0, 0.0, curve.span_ms);
  const VoiceCursor late = cursor_for_voice(curve, 30000.0, 0.0, curve.span_ms);
  CHECK(std::fabs(soon.release_from_ms - late.release_from_ms) < 1e-9);
  CHECK(std::fabs(soon.ms - late.ms) < 1e-9);
}

/// Pins that silent_for_ms is 0 while still falling and starts counting only
/// once the voice passes the release's silence point.
void test_a_voice_reports_how_long_it_has_been_silent() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = build_envelope_curve(op, kMiddleC);
  CHECK(std::isfinite(curve.release_silence_ms));

  const double att = curve_out_at_ms(curve.held, 0.0);
  const double entry = release_entry_ms(curve.release, att);
  const double to_silence = curve.release_silence_ms - entry;
  CHECK(to_silence > 0.0);
  CHECK(cursor_for_voice(curve, to_silence * 0.5, to_silence * 0.5,
                         curve.span_ms)
            .silent_for_ms == 0.0);
  const VoiceCursor gone = cursor_for_voice(curve, to_silence + 100.0,
                                            to_silence + 100.0, curve.span_ms);
  CHECK(near_rel(gone.silent_for_ms, 100.0, 0.05));
}

/// Pins that a held SSG loop's cursor goes round with the sound (wrapping
/// over the drawn periods) instead of sticking at the right-hand edge, while
/// a non-looping cursor simply runs off the axis.
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

  const VoiceCursor early =
      cursor_for_voice(c, first_fold * 0.5, -1.0, c.span_ms);
  CHECK(near_rel(early.ms, first_fold * 0.5, 0.001));

  const double beyond = last_fold + period * 2.5;
  const VoiceCursor wrapped = cursor_for_voice(c, beyond, -1.0, c.span_ms);
  CHECK(wrapped.ms < last_fold);
  CHECK(wrapped.ms >= first_fold);
  const VoiceCursor next =
      cursor_for_voice(c, beyond + (last_fold - first_fold), -1.0, c.span_ms);
  CHECK(near_rel(next.ms, wrapped.ms, 0.001));

  CHECK(near_rel(wrapped_into_loop_ms(c, beyond, c.span_ms), wrapped.ms,
                 0.001));

  OperatorParams plain = op;
  plain.ssg = 0;
  const EnvelopeCurve pc = build_envelope_curve(plain, kMiddleC);
  CHECK(wrapped_into_loop_ms(pc, pc.span_ms * 5.0, pc.span_ms) ==
        pc.span_ms * 5.0);
  const VoiceCursor gone =
      cursor_for_voice(pc, pc.span_ms * 5.0, -1.0, pc.span_ms);
  CHECK(gone.ms > pc.span_ms || pc.held_parked);
}

/// Pins that a voice's held_to_ms tracks exactly as far as the note has got,
/// never past the axis, freezes at key-off, and reaches the whole axis once a
/// loop has come round.
void test_a_voice_draws_only_what_it_has_been_through() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);

  const VoiceCursor early = cursor_for_voice(c, 40.0, -1.0, c.span_ms);
  CHECK(near_rel(early.held_to_ms, 40.0, 0.001));

  const VoiceCursor beyond =
      cursor_for_voice(c, c.span_ms * 3.0, -1.0, c.span_ms);
  CHECK(near_rel(beyond.held_to_ms, c.span_ms, 0.001));

  const double key_off = 40.0;
  const VoiceCursor just_off = cursor_for_voice(c, key_off, 0.0, c.span_ms);
  const VoiceCursor long_off =
      cursor_for_voice(c, key_off + 500.0, 500.0, c.span_ms);
  CHECK(near_rel(just_off.held_to_ms, key_off, 0.001));
  CHECK(near_rel(long_off.held_to_ms, key_off, 0.001));

  OperatorParams loop = op;
  loop.ssg = ssg_bits(0);
  loop.sl = 15;
  loop.dr = 15;
  const EnvelopeCurve lc = build_envelope_curve(loop, kMiddleC);
  CHECK(lc.held.loop_hz > 0.0);
  const VoiceCursor round =
      cursor_for_voice(lc, lc.span_ms * 2.5, -1.0, lc.span_ms);
  CHECK(near_rel(round.held_to_ms, lc.span_ms, 0.001));
  CHECK(round.ms <= lc.span_ms);
}

/// Pins that a released voice's release_from_ms moves further along the
/// release trace the later the key comes up, and that the cursor position is
/// the entry point plus time elapsed since.
void test_a_released_voice_reports_where_its_release_begins() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve c = build_envelope_curve(op, kMiddleC);

  const VoiceCursor held = cursor_for_voice(c, 100.0, -1.0, c.span_ms);
  CHECK(held.release_from_ms < 0.0);
  CHECK(!held.released);

  const double early_off = c.attack_end_ms + 1.0;
  const VoiceCursor early =
      cursor_for_voice(c, early_off + 5.0, 5.0, c.span_ms);
  CHECK(early.released);
  CHECK(early.release_from_ms >= 0.0);

  const double late_off = c.decay_end_ms + 50.0;
  const VoiceCursor late = cursor_for_voice(c, late_off + 5.0, 5.0, c.span_ms);
  CHECK(late.release_from_ms > early.release_from_ms);

  CHECK(near_rel(late.ms, late.release_origin_ms + 5.0, 0.001));
}

// --------------------------------------------- the voice curve cache's key

/// Pins that two notes sharing a key-scale value share one cached curve (no
/// second simulation, no budget spent), that a different ksv is a different
/// entry gated by budget, and that sharing the reference's ksv costs nothing.
void test_two_notes_sharing_a_key_scale_value_share_a_curve() {
  OperatorParams op = worked_example();
  op.ks = 0;

  const NotePitch c3 = NotePitch::from_midi(48);
  const NotePitch c4 = NotePitch::from_midi(60);
  const NotePitch c5 = NotePitch::from_midi(72);
  CHECK(key_scale_value(op, c4) == key_scale_value(op, c5));
  CHECK(key_scale_value(op, c3) != key_scale_value(op, c4));

  const EnvelopeCurve reference = build_envelope_curve(op, kC0);

  VoiceCurveCache cache;
  int budget = 1;
  const EnvelopeCurve *first = cache.get(op, c4, reference, kC0, budget);
  CHECK(first != nullptr);
  CHECK(budget == 0); // one simulation, and it was paid for
  CHECK(cache.rebuild_count() == 1);
  CHECK(cache.size() == 1);
  CHECK(first != &reference);

  const EnvelopeCurve *second = cache.get(op, c5, reference, kC0, budget);
  CHECK(second == first);
  CHECK(budget == 0);
  CHECK(cache.rebuild_count() == 1);
  CHECK(cache.size() == 1);

  CHECK(cache.get(op, c3, reference, kC0, budget) == nullptr);
  CHECK(cache.rebuild_count() == 1);
  budget = 1;
  const EnvelopeCurve *third = cache.get(op, c3, reference, kC0, budget);
  CHECK(third != nullptr);
  CHECK(third != first);
  CHECK(cache.rebuild_count() == 2);
  CHECK(cache.size() == 2);

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

/// Pins that a voice curve is simulated across the axis it is drawn on (the
/// reference's), even though its own window is much narrower.
void test_a_voice_curve_covers_the_axis_it_is_drawn_on() {
  OperatorParams op = worked_example();
  op.ks = 3; // so a high note's envelope is far shorter than C0's
  const EnvelopeCurve reference = build_envelope_curve(op, kC0);
  VoiceCurveCache cache;
  int budget = 1;
  const EnvelopeCurve *voice =
      cache.get(op, NotePitch::from_midi(96), reference, kC0, budget);
  CHECK(voice != nullptr);
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

/// Pins that the same registers decay far faster high up the keyboard at
/// KS = 3, and a much smaller (but real) amount at KS = 0.
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

  OperatorParams flat = worked_example();
  flat.ks = 0;
  const double flat_low =
      build_envelope_curve(flat, NotePitch::from_midi(36)).decay_end_ms;
  const double flat_high =
      build_envelope_curve(flat, NotePitch::from_midi(96)).decay_end_ms;
  CHECK(flat_high < flat_low);
  CHECK(flat_high > flat_low * 0.5);
}

// ----------------------------------------------------------- the solvers

/// The phases a solver inverts from a LENGTH, so a test can put the same
/// question to all of them. The sustain is not one of them: its handle is
/// dragged up and down the sustain line rather than along the axis.
enum class Phase { Attack, Decay, Release };

const Phase kPhases[] = {Phase::Attack, Phase::Decay, Phase::Release};

const char *phase_name(Phase phase) {
  switch (phase) {
  case Phase::Attack:
    return "attack";
  case Phase::Decay:
    return "decay";
  case Phase::Release:
    break;
  }
  return "release";
}

int slowest_rate(Phase phase) { return phase == Phase::Release ? 0 : 1; }
int fastest_rate(Phase phase) { return phase == Phase::Release ? 15 : 31; }

/// The forward direction: how long that phase lasts with `value` in place.
double phase_ms(Phase phase, const OperatorParams &op, NotePitch pitch,
                int value) {
  OperatorParams probe = op;
  switch (phase) {
  case Phase::Attack:
    probe.ar = static_cast<uint8_t>(value);
    return phase_durations(probe, pitch).attack_ms;
  case Phase::Decay:
    probe.dr = static_cast<uint8_t>(value);
    return phase_durations(probe, pitch).decay_ms;
  case Phase::Release:
    break;
  }
  probe.rr = static_cast<uint8_t>(value);
  return ym2612_eg::graph::detail::release_ms(probe, pitch);
}

int solve_rate(Phase phase, const OperatorParams &op, NotePitch pitch,
               double target_ms) {
  switch (phase) {
  case Phase::Attack:
    return solve_attack_rate(op, pitch, target_ms);
  case Phase::Decay:
    return solve_decay_rate(op, pitch, target_ms);
  case Phase::Release:
    break;
  }
  return solve_release_rate(op, pitch, target_ms);
}

/// The patches every solver test is put through: both key-scaling extremes,
/// three octaves, the sustain level at both ends and in the middle, a
/// non-zero total level, and SSG-EG both off and on (which moves where the
/// sustain and the release stop, and quadruples their increments).
std::vector<OperatorParams> solver_patches() {
  std::vector<OperatorParams> patches;
  for (int ks = 0; ks < 4; ++ks) {
    for (int sl : {0, 4, 15}) {
      for (int tl : {0, 40}) {
        for (int ssg : {0x00, 0x08}) {
          OperatorParams op = adsr(25, 12, sl, 6, 8, ks);
          op.tl = static_cast<uint8_t>(tl);
          op.ssg = static_cast<uint8_t>(ssg);
          patches.push_back(op);
        }
      }
    }
  }
  return patches;
}

const NotePitch kSolverNotes[] = {NotePitch::from_midi(36),
                                  NotePitch::from_midi(60),
                                  NotePitch::from_midi(84)};

/// Pins the round trip: the duration a register value produces solves back to
/// a value that produces that same duration. Where two values quantise to one
/// duration -- which the increment table does wherever the effective rate
/// saturates -- either is a correct answer, so the durations are compared
/// rather than the values.
void test_every_rate_solves_back_to_the_value_it_came_from() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      for (Phase phase : kPhases) {
        for (int v = slowest_rate(phase); v <= fastest_rate(phase); ++v) {
          const double want = phase_ms(phase, op, pitch, v);
          const int got = solve_rate(phase, op, pitch, want);
          CHECK(got >= slowest_rate(phase));
          CHECK(got <= fastest_rate(phase));
          if (phase_ms(phase, op, pitch, got) != want) {
            std::cerr << "\n    (" << phase_name(phase)
                      << " ks=" << static_cast<int>(op.ks)
                      << " sl=" << static_cast<int>(op.sl)
                      << " ssg=" << static_cast<int>(op.ssg) << " value " << v
                      << " -> " << got << ")";
            CHECK(false);
          }
        }
      }
    }
  }
}

/// Pins that the axis and the register move together: dragging a handle to
/// the right, i.e. asking for a longer phase, never answers with a faster
/// rate.
void test_a_longer_target_never_asks_for_a_faster_rate() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      for (Phase phase : kPhases) {
        int previous = fastest_rate(phase) + 1;
        for (int step = 0; step <= 400; ++step) {
          // 0.001 ms to 100 s, the whole reachable range and then some.
          const double target_ms = 0.001 * std::pow(10.0, step * 8.0 / 400.0);
          const int got = solve_rate(phase, op, pitch, target_ms);
          CHECK(got <= previous);
          previous = got;
        }
      }
    }
  }
}

/// Pins that a length solver never answers with the value that means "never":
/// AR and DR of 0 are a phase that does not advance, which is the caller's
/// decision to make rather than something a drag can land on.
void test_a_rate_solver_never_answers_the_rate_that_never_advances() {
  const double targets[] = {0.0,
                            -1.0,
                            1e-6,
                            1.0,
                            250.0,
                            1e6,
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()};
  for (const OperatorParams &op : solver_patches()) {
    for (double target : targets) {
      CHECK(solve_attack_rate(op, kMiddleC, target) >= 1);
      CHECK(solve_decay_rate(op, kMiddleC, target) >= 1);
      CHECK(solve_attack_rate(op, kMiddleC, target) <= 31);
      CHECK(solve_decay_rate(op, kMiddleC, target) <= 31);
      CHECK(solve_release_rate(op, kMiddleC, target) <= 15);
    }
  }
}

/// Pins both ends of the range: a target longer than the slowest rate can
/// manage is the slowest rate, a target shorter than the fastest is the
/// fastest -- or, where several values share that shortest duration, one of
/// them.
void test_a_target_off_either_end_lands_on_the_end() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      for (Phase phase : kPhases) {
        const int slowest = slowest_rate(phase);
        const int fastest = fastest_rate(phase);
        CHECK(solve_rate(phase, op, pitch, 1e9) == slowest);
        const int quick = solve_rate(phase, op, pitch, 1e-9);
        CHECK(phase_ms(phase, op, pitch, quick) ==
              phase_ms(phase, op, pitch, fastest));
      }
    }
  }
}

/// Pins that a target which is not a length of time asks for the fastest
/// rate. Zero, negative, infinite and NaN all arrive from a drag that has
/// gone off the axis; none of them is a reason to answer slowly.
void test_a_target_that_is_not_a_time_asks_for_the_fastest() {
  const double targets[] = {0.0, -0.0, -12.5,
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()};
  const OperatorParams op = worked_example();
  for (double target : targets) {
    CHECK_EQ(solve_attack_rate(op, kMiddleC, target), 31);
    CHECK_EQ(solve_decay_rate(op, kMiddleC, target), 31);
    CHECK_EQ(solve_release_rate(op, kMiddleC, target), 15);
  }
}

/// Pins the metric: no other value sits closer to the target in RATIO, which
/// is what a logarithmic time axis makes "closest" mean.
void test_no_other_rate_is_nearer_the_target_in_ratio() {
  for (const OperatorParams &op : solver_patches()) {
    for (Phase phase : kPhases) {
      for (int step = 0; step <= 60; ++step) {
        const double target_ms = 0.05 * std::pow(10.0, step * 6.0 / 60.0);
        const int got = solve_rate(phase, op, kMiddleC, target_ms);
        const double got_ms = phase_ms(phase, op, kMiddleC, got);
        if (!(got_ms > 0.0)) {
          continue; // a phase of no length has no ratio to the target
        }
        const double chosen = std::fabs(std::log(got_ms / target_ms));
        for (int v = slowest_rate(phase); v <= fastest_rate(phase); ++v) {
          const double ms = phase_ms(phase, op, kMiddleC, v);
          if (ms > 0.0) {
            CHECK(std::fabs(std::log(ms / target_ms)) >= chosen - 1e-12);
          }
        }
      }
    }
  }
}

/// Pins where the release stops. A release and a sustain with SL = 0 cover
/// exactly the same ground -- full volume to the level the chip cuts the
/// output at, 0x3F0 or SSG-EG's fold at 0x200 -- so at the same effective
/// rate, which RR reaches as 2*RR+1, they are the same length to the bit.
void test_a_release_covers_the_same_ground_as_a_sustain() {
  for (int ks = 0; ks < 4; ++ks) {
    for (int ssg : {0x00, 0x08}) {
      for (const NotePitch &pitch : kSolverNotes) {
        for (int rr = 0; rr < 8; ++rr) {
          OperatorParams op = adsr(25, 12, 0, 2 * rr + 1, rr, ks);
          op.ssg = static_cast<uint8_t>(ssg);
          CHECK(ym2612_eg::graph::detail::release_ms(op, pitch) ==
                phase_durations(op, pitch).sustain_ms);
        }
      }
    }
  }
}

/// Pins the closed form the release solver inverts against the release the
/// graph actually draws. They differ only by where the shared counter
/// happened to be when the key came up -- an increment either way, which on
/// the shortest SSG-EG releases is a percent or two of the whole -- and by
/// what the polyline's own resolution can carry.
void test_the_release_length_matches_the_release_that_is_drawn() {
  const double eg_tick_ms = 1000.0 / eg_rate_hz(kNtscClockHz);
  for (int ks = 0; ks < 4; ++ks) {
    for (int ssg : {0x00, 0x08, 0x0C}) {
      for (const NotePitch &pitch : kSolverNotes) {
        for (int rr = 0; rr < 16; ++rr) {
          OperatorParams op = adsr(25, 12, 4, 6, rr, ks);
          op.ssg = static_cast<uint8_t>(ssg);
          const EnvelopeCurve curve = build_envelope_curve(op, pitch);
          // Only the releases that finish inside their budget: one cut off at
          // the ceiling has no length of its own to compare.
          if (curve.release_content_ms >= release_max_ms() * 0.99) {
            continue;
          }
          CHECK_ABS(ym2612_eg::graph::detail::release_ms(op, pitch),
                    curve.release_content_ms,
                    std::max(0.02 * curve.release_content_ms,
                             1.5 * eg_tick_ms));
        }
      }
    }
  }
}

/// Pins which way a tie falls. The effective rate saturates at 63, so at
/// KS = 0 on a low note DR of 30 and 31 decay at exactly the same speed; the
/// slower value wins, which is what keeps dragging a handle outward from
/// sticking to the end of the range.
void test_a_tie_goes_to_the_slower_rate() {
  const OperatorParams op = adsr(25, 12, 4, 6, 8, 0);
  const NotePitch low = NotePitch::from_midi(12);
  CHECK_EQ(key_scale_value(op, low), 0);

  const double decay = phase_ms(Phase::Decay, op, low, 31);
  CHECK(phase_ms(Phase::Decay, op, low, 30) == decay);
  CHECK_EQ(solve_decay_rate(op, low, decay), 30);
}

/// Pins that no value is shadowed: sweeping a handle across the axis lands on
/// every rate whose phase length is its own, and only those -- the range is
/// divided between them rather than spent on a few.
void test_dragging_across_the_axis_reaches_every_rate() {
  const OperatorParams op = adsr(25, 12, 4, 6, 8, 0);
  for (Phase phase : kPhases) {
    const int slowest = slowest_rate(phase);
    const int fastest = fastest_rate(phase);
    const double longest = phase_ms(phase, op, kMiddleC, slowest);
    bool seen[32] = {false};
    int distinct = 0;
    for (int step = 0; step <= 2000; ++step) {
      // The whole reachable range, walked evenly on a log axis.
      const double target_ms = longest * std::pow(1e-6, step / 2000.0);
      const int got = solve_rate(phase, op, kMiddleC, target_ms);
      if (!seen[got]) {
        seen[got] = true;
        ++distinct;
      }
    }
    // Every value except those whose duration another value already shares.
    int reachable = 0;
    for (int v = slowest; v <= fastest; ++v) {
      const double ms = phase_ms(phase, op, kMiddleC, v);
      if (v == slowest || ms != phase_ms(phase, op, kMiddleC, v - 1)) {
        ++reachable;
      }
    }
    // At most a couple of values are lost to a shared duration, so this is a
    // claim about nearly the whole range rather than about a handful of it.
    CHECK(reachable >= fastest - slowest - 1);
    CHECK(distinct == reachable);
  }
}

/// Pins that the note and the key-scaling reach the answer: the same target
/// asks for a different rate once the effective rate moves under it.
void test_the_note_and_the_key_scaling_change_which_rate_a_time_means() {
  OperatorParams scaled = adsr(25, 12, 4, 6, 8, 3);
  const NotePitch low = NotePitch::from_midi(36);
  const NotePitch high = NotePitch::from_midi(96);
  CHECK(solve_decay_rate(scaled, low, 200.0) !=
        solve_decay_rate(scaled, high, 200.0));
  CHECK(solve_release_rate(scaled, low, 200.0) !=
        solve_release_rate(scaled, high, 200.0));

  OperatorParams unscaled = scaled;
  unscaled.ks = 0;
  CHECK(solve_decay_rate(scaled, high, 200.0) !=
        solve_decay_rate(unscaled, high, 200.0));

  // ... and with KS = 0 the same note still shifts the answer, because the
  // key scale value is the keycode's top bits rather than nothing at all.
  CHECK(solve_decay_rate(unscaled, NotePitch::from_midi(0), 200.0) !=
        solve_decay_rate(unscaled, NotePitch::from_midi(120), 200.0));
}

// ------------------------------------------ the sustain, solved from a level

/// The forward direction the sustain solver inverts: where a sustain at `sr`
/// stands at that instant, in the units the graph draws.
double sustain_out(const OperatorParams &op, NotePitch pitch, int sr,
                   double elapsed_ms) {
  OperatorParams probe = op;
  probe.sr = static_cast<uint8_t>(sr);
  return ym2612_eg::graph::detail::sustain_out_at_ms(probe, pitch, elapsed_ms);
}

/// Where a sustain at `sr` is probed: early in its fall, half way down, and
/// past the point it has come to rest. A rate with no length of its own -- one
/// that never advances, or a sustain with nowhere to fall -- borrows a spread
/// of times instead.
std::vector<double> sustain_probe_times(const OperatorParams &op,
                                        NotePitch pitch, int sr) {
  OperatorParams probe = op;
  probe.sr = static_cast<uint8_t>(sr);
  const double whole = phase_durations(probe, pitch).sustain_ms;
  if (!std::isfinite(whole) || !(whole > 0.0)) {
    return {1.0, 250.0, 10000.0};
  }
  return {0.05 * whole, 0.5 * whole, 2.0 * whole};
}

/// Pins the round trip: the level a sustain rate has fallen to at some instant
/// solves back to that rate. Where two rates draw the same level -- TL
/// flattening the bottom of the scale, or both already at rest -- either is a
/// correct answer, so the levels are compared rather than the values.
void test_every_sustain_rate_solves_back_from_the_level_it_reaches() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      for (int sr = 0; sr <= 31; ++sr) {
        for (const double elapsed : sustain_probe_times(op, pitch, sr)) {
          const double want = sustain_out(op, pitch, sr, elapsed);
          const int got = solve_sustain_rate(op, pitch, elapsed, want);
          CHECK(got >= 0);
          CHECK(got <= 31);
          if (sustain_out(op, pitch, got, elapsed) != want) {
            std::cerr << "\n    (ks=" << static_cast<int>(op.ks)
                      << " sl=" << static_cast<int>(op.sl)
                      << " tl=" << static_cast<int>(op.tl)
                      << " ssg=" << static_cast<int>(op.ssg) << " sr " << sr
                      << " at " << elapsed << " ms -> " << got << ")";
            CHECK(false);
          }
        }
      }
    }
  }
}

/// Pins both ends of the drag: a level the sustain has not fallen from is the
/// hold, SR = 0, at every instant; and the bottom of the scale is the first
/// rate to have got that far, every slower one still being above it.
void test_the_ends_of_the_drag_are_the_hold_and_the_first_rate_down() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      const double sustain_level = sustain_out(op, pitch, 0, 0.0);
      for (const double elapsed : {1.0, 50.0, 1000.0, 20000.0}) {
        CHECK_EQ(solve_sustain_rate(op, pitch, elapsed, sustain_level), 0);

        const int down =
            solve_sustain_rate(op, pitch, elapsed, kMaxAttenuation);
        const double deepest = sustain_out(op, pitch, down, elapsed);
        CHECK(deepest == sustain_out(op, pitch, 31, elapsed));
        for (int slower = 0; slower < down; ++slower) {
          CHECK(sustain_out(op, pitch, slower, elapsed) < deepest);
        }
      }
    }
  }
}

/// Pins that the handle and the register move together: at one instant, a
/// quieter target never answers with a slower rate.
void test_a_quieter_level_never_asks_for_a_slower_rate() {
  for (const OperatorParams &op : solver_patches()) {
    for (const NotePitch &pitch : kSolverNotes) {
      for (const double elapsed : {2.0, 40.0, 600.0, 9000.0}) {
        int previous = 0;
        for (int step = 0; step <= 400; ++step) {
          const double target = kMaxAttenuation * step / 400.0;
          const int got = solve_sustain_rate(op, pitch, elapsed, target);
          CHECK(got >= previous);
          previous = got;
        }
      }
    }
  }
}

/// Pins the metric: no other rate's level sits closer to the target, and
/// where one is exactly as close the slower value wins.
void test_no_other_sustain_rate_sits_nearer_the_level() {
  const OperatorParams op = adsr(25, 12, 4, 6, 8, 0);
  for (const NotePitch &pitch : kSolverNotes) {
    for (const double elapsed : {5.0, 120.0, 3000.0}) {
      for (int step = 0; step <= 200; ++step) {
        const double target = kMaxAttenuation * step / 200.0;
        const int got = solve_sustain_rate(op, pitch, elapsed, target);
        const double chosen =
            std::fabs(sustain_out(op, pitch, got, elapsed) - target);
        for (int sr = 0; sr <= 31; ++sr) {
          const double distance =
              std::fabs(sustain_out(op, pitch, sr, elapsed) - target);
          CHECK(distance > chosen || (distance == chosen && sr >= got));
        }
      }
    }
  }
}

/// Pins that the target is read in OUTPUT units: TL moves the whole scale
/// under it, so the same level at the same instant names a different rate.
void test_total_level_moves_which_rate_a_level_means() {
  OperatorParams loud = adsr(25, 12, 0, 6, 8, 0);
  loud.tl = 0;
  OperatorParams quiet = loud;
  quiet.tl = 40;
  CHECK(solve_sustain_rate(loud, kMiddleC, 200.0, 600.0) !=
        solve_sustain_rate(quiet, kMiddleC, 200.0, 600.0));
}

/// Pins the edges of both arguments: an instant that is not one is a sustain
/// that has not advanced, which every rate answers alike; and a level off the
/// scale clamps onto its ends.
void test_a_level_or_an_instant_off_the_scale_clamps() {
  const OperatorParams op = adsr(25, 12, 4, 6, 8, 0);
  const double fallen = sustain_out(op, kMiddleC, 20, 300.0);
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double elapsed : {-1.0, -1e9, infinity, -infinity, nan, 0.0}) {
    CHECK_EQ(solve_sustain_rate(op, kMiddleC, elapsed, fallen), 0);
    CHECK_EQ(solve_sustain_rate(op, kMiddleC, elapsed, 0.0), 0);
  }

  const int bottom = solve_sustain_rate(op, kMiddleC, 300.0, kMaxAttenuation);
  CHECK_EQ(solve_sustain_rate(op, kMiddleC, 300.0, 1e9), bottom);
  CHECK_EQ(solve_sustain_rate(op, kMiddleC, 300.0, infinity), bottom);
  const int top = solve_sustain_rate(op, kMiddleC, 300.0, 0.0);
  CHECK_EQ(solve_sustain_rate(op, kMiddleC, 300.0, -1e9), top);
  CHECK_EQ(solve_sustain_rate(op, kMiddleC, 300.0, -infinity), top);
  CHECK_EQ(solve_sustain_rate(op, kMiddleC, 300.0, nan), top);
}

/// Pins TL as the top 7 bits of the 10-bit attenuation: one step is 8 units,
/// halves round away from zero, and anything off the scale clamps.
void test_total_level_rounds_to_the_nearest_step() {
  for (int tl = 0; tl < 128; ++tl) {
    CHECK_EQ(solve_total_level(tl * 8.0), tl);
    CHECK_EQ(solve_total_level(tl * 8.0 + 3.0), tl);
  }
  CHECK_EQ(solve_total_level(4.0), 1);   // the midpoint rounds up
  CHECK_EQ(solve_total_level(3.9), 0);
  CHECK_EQ(solve_total_level(12.0), 2);
  CHECK_EQ(solve_total_level(-0.0), 0);
  CHECK_EQ(solve_total_level(-500.0), 0);
  CHECK_EQ(solve_total_level(1023.0), 127);
  CHECK_EQ(solve_total_level(1e9), 127);
  CHECK_EQ(solve_total_level(std::numeric_limits<double>::infinity()), 127);
  CHECK_EQ(solve_total_level(-std::numeric_limits<double>::infinity()), 0);
  CHECK_EQ(solve_total_level(std::numeric_limits<double>::quiet_NaN()), 0);
}

/// Pins SL against the levels it really has -- 32 units apart, except SL = 15
/// at 0x3E0 -- measured from wherever TL has put the operator's ceiling.
void test_sustain_level_rounds_to_the_nearest_level() {
  OperatorParams op = worked_example();
  op.tl = 0;
  for (int sl = 0; sl < 16; ++sl) {
    CHECK_EQ(solve_sustain_level(op, sustain_attenuation(sl)), sl);
  }
  CHECK_EQ(solve_sustain_level(op, 100.0), 3);  // 96 vs 128
  CHECK_EQ(solve_sustain_level(op, 112.0), 3);  // the midpoint stays louder
  CHECK_EQ(solve_sustain_level(op, 113.0), 4);
  CHECK_EQ(solve_sustain_level(op, 720.0), 14); // 448 vs 992
  CHECK_EQ(solve_sustain_level(op, 721.0), 15);
  CHECK_EQ(solve_sustain_level(op, -1000.0), 0);
  CHECK_EQ(solve_sustain_level(op, 1e9), 15);
  CHECK_EQ(solve_sustain_level(op, std::numeric_limits<double>::quiet_NaN()),
           0);

  // TL raises the whole scale: the same attenuation now names a louder SL.
  op.tl = 8; // 64 units
  for (int sl = 0; sl < 16; ++sl) {
    CHECK_EQ(solve_sustain_level(op, sustain_attenuation(sl) + 64.0), sl);
  }
  CHECK_EQ(solve_sustain_level(op, 160.0), 3); // 96 + 64
  CHECK_EQ(solve_sustain_level(op, 0.0), 0);
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

  RUN_TEST(test_every_rate_solves_back_to_the_value_it_came_from);
  RUN_TEST(test_a_longer_target_never_asks_for_a_faster_rate);
  RUN_TEST(test_a_rate_solver_never_answers_the_rate_that_never_advances);
  RUN_TEST(test_a_target_off_either_end_lands_on_the_end);
  RUN_TEST(test_a_target_that_is_not_a_time_asks_for_the_fastest);
  RUN_TEST(test_no_other_rate_is_nearer_the_target_in_ratio);
  RUN_TEST(test_a_tie_goes_to_the_slower_rate);
  RUN_TEST(test_a_release_covers_the_same_ground_as_a_sustain);
  RUN_TEST(test_the_release_length_matches_the_release_that_is_drawn);
  RUN_TEST(test_dragging_across_the_axis_reaches_every_rate);
  RUN_TEST(test_the_note_and_the_key_scaling_change_which_rate_a_time_means);

  RUN_TEST(test_every_sustain_rate_solves_back_from_the_level_it_reaches);
  RUN_TEST(test_the_ends_of_the_drag_are_the_hold_and_the_first_rate_down);
  RUN_TEST(test_a_quieter_level_never_asks_for_a_slower_rate);
  RUN_TEST(test_no_other_sustain_rate_sits_nearer_the_level);
  RUN_TEST(test_total_level_moves_which_rate_a_level_means);
  RUN_TEST(test_a_level_or_an_instant_off_the_scale_clamps);

  RUN_TEST(test_total_level_rounds_to_the_nearest_step);
  RUN_TEST(test_sustain_level_rounds_to_the_nearest_level);

  return testing::summary();
}
