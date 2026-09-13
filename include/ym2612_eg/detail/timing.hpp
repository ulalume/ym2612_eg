#pragma once

// Answers about an envelope's shape without drawing it: how long each phase of
// a key-held note lasts and how fast an SSG-EG loop runs. A rate that never
// advances holds forever, and says so.

#include "constants.hpp"
#include "simulator.hpp"
#include "tables.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace ym2612_eg {
namespace detail {

/// One turn of a rate's row of the increment table.
inline int row_sum(int rate) {
  int sum = 0;
  for (int i = 0; i < 8; ++i) {
    sum += kIncTable[rate][i];
  }
  return sum;
}

/// Rows 0 and 1 are all zero, which is the chip's way of saying this phase
/// never advances; only R = 0 reaches them.
inline bool rate_advances(int rate) { return row_sum(rate) != 0; }

/// The attenuation one EG tick adds, on average, at this effective rate.
/// increment_at() indexes kIncTable with three bits of the free-running EG
/// counter taken from above rate_shift(), so across one turn of those bits
/// each of the eight entries lands exactly once and their mean is the true
/// gain per tick -- the shift being how many ticks each entry is held for.
inline double atten_per_eg_tick(int rate, bool ssg) {
  const double mean = static_cast<double>(row_sum(rate)) / 8.0;
  const double per_tick = mean / static_cast<double>(1 << rate_shift(rate));
  // SSG-EG quadruples every post-attack increment.
  return ssg ? per_tick * 4.0 : per_tick;
}

/// How long a phase that is linear in attenuation takes to climb from
/// `from_att` to `to_att`, in ms.  Decay, sustain and release all are: they
/// add a fixed increment per tick, so the duration is one division rather
/// than a simulation.  A rate that never advances takes forever, and says so.
inline double linear_phase_ms(int rate, int from_att, int to_att, bool ssg,
                              double eg_hz) {
  if (to_att <= from_att) {
    return 0.0;
  }
  const double per_tick = atten_per_eg_tick(rate, ssg);
  if (!(per_tick > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  const double ticks = static_cast<double>(to_att - from_att) / per_tick;
  return ticks * 1000.0 / eg_hz;
}

/// How long the attack after a key-on takes, in ms. It multiplies what is
/// left, `att += (~att * inc) >> 4`, so the recurrence is run rather than
/// divided.
inline double attack_ms(int rate, double eg_hz) {
  // A key-on snaps these straight to att = 0; the attack update guards on
  // `rate < 62`.
  if (rate >= 62) {
    return 0.0;
  }
  if (!rate_advances(rate)) {
    // Rates 0 and 1, i.e. AR = 0: the attack never finishes, so neither does
    // anything after it.
    return std::numeric_limits<double>::infinity();
  }
  const int shift = rate_shift(rate);
  int att = static_cast<int>(kMaxAttenuation);
  long long slots = 0;
  // The slowest table row alternates {0, 1}, which needs about 214 slots from
  // 0x3FF; the bound is only here so a future table could not hang a frame.
  constexpr long long kSlotLimit = 4096;
  for (; att > 0 && slots < kSlotLimit; ++slots) {
    const int inc = kIncTable[rate][slots & 7];
    if (inc != 0) {
      // Arithmetic shift of a negative value, exactly as the simulator does it.
      att += (~att * inc) >> 4;
    }
  }
  return static_cast<double>(slots << shift) * 1000.0 / eg_hz;
}

/// How many periods of a held SSG-EG loop sample_curve() draws before it
/// stops, and so how many ramps the loop period is measured over.
inline constexpr int kMeasuredLoopPeriods = 5;

/// The mean fold-to-fold interval in output samples, skipping the first, which
/// still carries the key-on; 0 with fewer than three folds.
inline double mean_fold_interval(const std::vector<uint64_t> &folds) {
  if (folds.size() < 3) {
    return 0.0;
  }
  double sum = 0.0;
  size_t n = 0;
  for (size_t k = 2; k < folds.size(); ++k) {
    sum += static_cast<double>(folds[k] - folds[k - 1]);
    ++n;
  }
  return sum / static_cast<double>(n);
}

/// A held envelope as sample_curve() steps it: the labels of the samples its
/// first Attack -> Decay and Decay -> Sustain land on and where the level first
/// runs out of scale after the attack (-1 for never), and every SSG-EG fold.
struct HeldRun {
  int64_t attack_end = -1;
  int64_t decay_end = -1;
  int64_t ramp_end = -1;
  std::vector<uint64_t> folds;
  bool at_rest = false;
};

/// Keys on at `counter_phase` from `start_att` and follows the envelope to
/// `max_folds` folds -- with 0, to the end of its first ramp -- unless it comes
/// to rest first.
inline HeldRun run_held(const OperatorParams &op, NotePitch pitch,
                        uint16_t counter_phase, uint16_t start_att,
                        uint32_t max_folds) {
  HeldRun run;
  EgSimulator sim(op, pitch);
  sim.reset(counter_phase, start_att);
  sim.key_on();
  const int end_att = (op.ssg & 0x08) != 0 ? kSsgFoldAttenuation
                                           : kCutAttenuation;
  constexpr uint64_t kSampleLimit = uint64_t{1} << 40;
  for (uint64_t i = 0; i < kSampleLimit;) {
    uint32_t n = sim.skippable_samples();
    if (n == 0) {
      uint16_t first = 0, second = 0;
      n = sim.alternating_samples(first, second);
    }
    if (sim.is_static() || n >= kUnboundedSkip) {
      run.at_rest = true;
      break;
    }
    if (n > 0) {
      sim.skip(n);
      i += n;
      continue;
    }
    sim.step();
    const uint32_t ev = sim.step_events();
    const int64_t label = static_cast<int64_t>(i) + 1;
    if ((ev & kEvAttackEnd) && run.attack_end < 0)
      run.attack_end = label;
    if ((ev & kEvDecayEnd) && run.decay_end < 0)
      run.decay_end = label;
    if (run.attack_end >= 0 && run.ramp_end < 0 && sim.attenuation() >= end_att)
      run.ramp_end = label;
    if (ev & kEvSsgFold)
      run.folds.push_back(i);
    ++i;
    if (max_folds == 0 ? run.ramp_end >= 0 : run.folds.size() >= max_folds)
      break;
  }
  return run;
}

/// Whether the counter decides if a decay from 0 lands in the sustain window:
/// with SSG-EG, a row whose 4x steps mix 32 with smaller ones (DR rates 57-59)
/// can step past a window short of the fold.
inline bool window_can_be_skipped(const OperatorParams &op, NotePitch pitch) {
  const int sustain = sustain_attenuation(op.sl);
  if ((op.ssg & 0x08) == 0 || sustain == 0 || sustain >= kSsgFoldAttenuation)
    return false;
  const int rate = effective_rate(op.dr & 0x1F, key_scale_value(op, pitch));
  bool full = false, smaller = false;
  for (int i = 0; i < 8; ++i) {
    full = full || kIncTable[rate][i] == 8;
    smaller = smaller || kIncTable[rate][i] != 8;
  }
  return full && smaller;
}

/// The mean ramp of a held SSG-EG loop in output samples, measured as
/// sample_curve() measures it; infinite when the loop stops before three folds.
inline double loop_ramp_samples(const OperatorParams &op, NotePitch pitch,
                                uint16_t start_att) {
  const uint32_t ramps = (op.ssg & 0x02) != 0 ? 2 : 1;
  const HeldRun run = run_held(op, pitch, kCurveCounterPhase, start_att,
                               kMeasuredLoopPeriods * ramps);
  const double mean = mean_fold_interval(run.folds);
  return mean > 0.0 ? mean : std::numeric_limits<double>::infinity();
}

} // namespace detail

/// How long each phase of a key-held envelope lasts, in ms. A phase whose
/// effective rate never advances lasts forever and says so, and infinity is
/// contagious through the sums below -- which is right: a phase that never
/// ends means the ones after it never start.
struct PhaseDurations {
  double attack_ms = 0.0;
  /// Full volume down to the sustain level -- or, with SSG-EG, to the fold at
  /// 0x200 when the decay steps past the sustain window.
  double decay_ms = 0.0;
  /// The sustain level the rest of the way to silence -- or, with SSG-EG
  /// enabled, to the fold at 0x200.
  double sustain_ms = 0.0;

  /// Where the sustain begins: the last structural feature the envelope has.
  double sustain_start_ms() const { return attack_ms + decay_ms; }
  /// The whole course of the held envelope, key-on to silence.
  double lifetime_ms() const { return sustain_start_ms() + sustain_ms; }
};

/// The phase durations of `op` at `pitch`, with the key never released.  Where
/// the counter decides whether the decay lands in the sustain window, the first
/// ramp is run from kCurveCounterPhase.
inline PhaseDurations phase_durations(const OperatorParams &op, NotePitch pitch,
                                      double clock_hz = kNtscClockHz) {
  PhaseDurations phases;
  if (detail::window_can_be_skipped(op, pitch)) {
    const detail::HeldRun run = detail::run_held(op, pitch, kCurveCounterPhase,
                                                 kMaxAttenuation, 0);
    const double ms = 1000.0 / sample_rate_hz(clock_hz);
    const double forever = std::numeric_limits<double>::infinity();
    if (run.attack_end < 0) {
      phases.attack_ms = forever;
      return phases;
    }
    phases.attack_ms = static_cast<double>(run.attack_end) * ms;
    const int64_t decay_end = run.decay_end >= 0 ? run.decay_end : run.ramp_end;
    if (decay_end < 0) {
      phases.decay_ms = forever;
      return phases;
    }
    phases.decay_ms = static_cast<double>(decay_end - run.attack_end) * ms;
    if (run.decay_end >= 0) {
      phases.sustain_ms =
          run.ramp_end >= 0
              ? static_cast<double>(run.ramp_end - run.decay_end) * ms
              : forever;
    }
    return phases;
  }

  const int ksv = key_scale_value(op, pitch);
  const bool ssg = (op.ssg & 0x08) != 0;
  const double eg_hz = eg_rate_hz(clock_hz);
  // Where the held envelope runs out of scale: the output is cut dead at 0x3F0,
  // and with SSG-EG the fold and the hold latch are at 0x200.
  const int end_att = ssg ? static_cast<int>(kSsgFoldAttenuation)
                          : static_cast<int>(kCutAttenuation);
  const int sustain_att = std::min(sustain_attenuation(op.sl), end_att);

  const int ar = detail::effective_rate(op.ar & 0x1F, ksv);
  const int dr = detail::effective_rate(op.dr & 0x1F, ksv);
  const int sr = detail::effective_rate(op.sr & 0x1F, ksv);

  phases.attack_ms = detail::attack_ms(ar, eg_hz);
  phases.decay_ms = detail::linear_phase_ms(dr, 0, sustain_att, ssg, eg_hz);
  phases.sustain_ms =
      detail::linear_phase_ms(sr, sustain_att, end_att, ssg, eg_hz);
  return phases;
}

/// The visible period of an SSG-EG loop at `pitch`, in ms, as sample_curve()
/// measures a held key.  Zero for a mode that latches instead of looping, and
/// infinite for a loop that stops before its third fold (SsgNeverLoops).
inline double ssg_loop_period_ms(const OperatorParams &op, NotePitch pitch,
                                 double clock_hz = kNtscClockHz) {
  // Enabled and hold clear: the ramp restarts instead of latching.
  const bool loops = (op.ssg & 0x08) != 0 && (op.ssg & 0x01) == 0;
  if (!loops) {
    return 0.0;
  }
  // The alternating modes invert on every fold: two ramps to a period.
  const double ramps_per_period = (op.ssg & 0x02) != 0 ? 2.0 : 1.0;
  return detail::loop_ramp_samples(op, pitch, kMaxAttenuation) *
         ramps_per_period * 1000.0 / sample_rate_hz(clock_hz);
}

} // namespace ym2612_eg
