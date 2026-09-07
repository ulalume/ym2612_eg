#pragma once

// Closed-form answers about an envelope's shape: how long each phase of a
// key-held note lasts, and how fast an SSG-EG loop runs. A rate of 0 really
// does hold forever, and says so exactly -- rather than reporting whatever a
// probe saw before giving up -- which is the difference between SR = 0 and
// SR = 31. The post-attack phases are linear in attenuation, so each is one
// division; the attack is not, so its recurrence is iterated instead.

#include "constants.hpp"
#include "simulator.hpp"
#include "tables.hpp"

#include <algorithm>
#include <limits>

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

/// How long the attack after a key-on takes, in ms. The attack is the one
/// phase that is not linear: it multiplies what is left,
/// `att += (~att * inc) >> 4`, so there is no closed form and the recurrence
/// is run instead. The attack an SSG-EG fold starts is different -- it
/// resumes from 0x200, and its length depends on where the climb before it
/// left the shared counter -- so ssg_ramp_ms() walks its own rather than
/// calling this.
inline double attack_ms(int rate, double eg_hz) {
  // key_on() snaps these straight to att = 0; eg_step() guards on `rate < 62`.
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
      // Arithmetic shift of a negative value, exactly as eg_step() does it.
      att += (~att * inc) >> 4;
    }
  }
  return static_cast<double>(slots << shift) * 1000.0 / eg_hz;
}

/**
 * One walk of the envelope's own recurrence, kept where the chip keeps it: on
 * the free-running 12-bit counter, which every phase shares.
 *
 * A phase does not get its own clock.  rate_shift() says how many of the
 * counter's low bits must be clear before this rate may act, and the three
 * bits above that choose the table entry -- so a phase inherits whatever
 * phase of the counter the phase before it left behind, and where a boundary
 * falls decides which increment lands next.
 */
struct EgWalk {
  int counter = 0;
  long long ticks = 0;

  /// Advance to the next tick this rate is allowed to act on, and answer with
  /// the increment it finds there.
  int next_increment(int rate) {
    const int shift = rate_shift(rate);
    const int mask = (1 << shift) - 1;
    const int step = mask == 0 ? 1 : (mask + 1 - (counter & mask));
    // The chip's counter skips 0 on overflow, making its period 4095 rather
    // than 4096.  That is one tick in four thousand, far below anything a
    // graph resolves, and modelling it would buy a slip nothing can see.
    counter = (counter + step) & 0x0FFF;
    ticks += step;
    return kIncTable[rate][(counter >> shift) & 7];
  }
};

/**
 * How long one SSG-EG ramp takes, in ms: the attack that follows a fold, then
 * the attenuation climbing back to the fold at 0x200 -- through the decay
 * rate to the sustain level and the sustain rate the rest of the way, at
 * SSG-EG's quadrupled increments. Infinite when a rate the ramp needs never
 * advances, because then there is no next fold.
 *
 * Walked slot by slot rather than divided like linear_phase_ms(): the decay
 * overshoots the sustain level by up to one increment, negligible over a
 * whole lifetime but enough to shift a single ramp's length noticeably. The
 * overshoot also depends on the counter, which a fold does not reset, so ramp
 * lengths settle into a short repeating cycle rather than one value -- the
 * first ramps are walked and discarded to reach it, and the rest averaged.
 */
inline double ssg_ramp_ms(int ar, int dr, int sr, int sustain_att,
                          double eg_hz) {
  constexpr int kFold = static_cast<int>(kSsgFoldAttenuation);
  constexpr int kSettlingRamps = 2;
  constexpr int kMeasuredRamps = 4;
  // Loose enough that no reachable patch meets it, tight enough that a future
  // table could not hang a frame.
  constexpr long long kSlotLimit = 8192;
  const double forever = std::numeric_limits<double>::infinity();
  // ssg_step()'s virtual key-on snaps an instant attack straight to 0,
  // exactly as key_on() does; anything slower resumes from where the fold
  // found it.
  const bool instant_attack = ar >= 62;
  if (!instant_attack && !rate_advances(ar)) {
    return forever;
  }

  EgWalk walk;
  int att = kFold;
  long long settled_ticks = 0;
  for (int ramp = 0; ramp < kSettlingRamps + kMeasuredRamps; ++ramp) {
    long long slots = 0;
    if (instant_attack) {
      att = 0;
    } else {
      while (att > 0) {
        if (++slots > kSlotLimit) {
          return forever;
        }
        const int inc = walk.next_increment(ar);
        if (inc != 0) {
          // Arithmetic shift of a negative value, as eg_step() does it.
          att += (~att * inc) >> 4;
        }
      }
    }
    while (att < kFold) {
      // The chip compares against the raw sustain level, not one clamped to
      // the fold, which is how SL = 15 (0x3E0) spends the whole ramp in decay.
      const int rate = att < sustain_att ? dr : sr;
      if (!rate_advances(rate) || ++slots > kSlotLimit) {
        return forever;
      }
      att += 4 * walk.next_increment(rate);
    }
    if (ramp == kSettlingRamps - 1) {
      settled_ticks = walk.ticks;
    }
  }
  return static_cast<double>(walk.ticks - settled_ticks) * 1000.0 /
         (kMeasuredRamps * eg_hz);
}

} // namespace detail

/// How long each phase of a key-held envelope lasts, in ms. A phase whose
/// effective rate never advances lasts forever and says so, and infinity is
/// contagious through the sums below -- which is right: a phase that never
/// ends means the ones after it never start.
struct PhaseDurations {
  double attack_ms = 0.0;
  /// Full volume down to the sustain level.  Zero when SL = 0, which the chip
  /// skips outright.
  double decay_ms = 0.0;
  /// The sustain level the rest of the way to silence -- or, with SSG-EG
  /// enabled, to the fold at 0x200.
  double sustain_ms = 0.0;

  /// Where the sustain begins: the last structural feature the envelope has.
  double sustain_start_ms() const { return attack_ms + decay_ms; }
  /// The whole course of the held envelope, key-on to silence.
  double lifetime_ms() const { return sustain_start_ms() + sustain_ms; }
};

/// The phase durations of `op` at `pitch`, with the key never released.
inline PhaseDurations phase_durations(const OperatorParams &op, NotePitch pitch,
                                      double clock_hz = kNtscClockHz) {
  const int ksv = key_scale_value(op, pitch);
  const bool ssg = (op.ssg & 0x08) != 0;
  const double eg_hz = eg_rate_hz(clock_hz);

  // Where the held envelope runs out of scale.  Without SSG-EG the chip cuts
  // the output dead the moment the attenuation reaches 0x3F0; with it, both
  // the fold and the hold latch happen at 0x200 instead.
  const int end_att = ssg ? static_cast<int>(kSsgFoldAttenuation) : 0x3F0;
  const int sustain_att = std::min(sustain_attenuation(op.sl), end_att);

  const int ar = detail::effective_rate(op.ar & 0x1F, ksv);
  const int dr = detail::effective_rate(op.dr & 0x1F, ksv);
  const int sr = detail::effective_rate(op.sr & 0x1F, ksv);

  PhaseDurations phases;
  phases.attack_ms = detail::attack_ms(ar, eg_hz);
  phases.decay_ms = detail::linear_phase_ms(dr, 0, sustain_att, ssg, eg_hz);
  phases.sustain_ms =
      detail::linear_phase_ms(sr, sustain_att, end_att, ssg, eg_hz);
  return phases;
}

/**
 * The visible period of an SSG-EG loop at `pitch`, in ms.
 *
 * Zero when the patch is not a looping mode -- SSG-EG off, or a hold mode,
 * which latches instead of folding. Infinite when it is a looping mode whose
 * ramp never finishes because a phase never advances (DR = 0 below the
 * sustain level, SR = 0 above it, or AR = 0 after the fold) -- the case
 * sample_curve() names SsgNeverLoops.
 *
 * One ramp is the attenuation climbing from 0 to the fold at 0x200 plus the
 * attack that follows; the alternating modes (bit 1 of the SSG register)
 * invert on every fold, so two ramps make one visible period.
 */
inline double ssg_loop_period_ms(const OperatorParams &op, NotePitch pitch,
                                 double clock_hz = kNtscClockHz) {
  // Enabled and hold clear: the ramp restarts instead of latching.
  const bool loops = (op.ssg & 0x08) != 0 && (op.ssg & 0x01) == 0;
  if (!loops) {
    return 0.0;
  }
  const int ksv = key_scale_value(op, pitch);
  const double ramp = detail::ssg_ramp_ms(
      detail::effective_rate(op.ar & 0x1F, ksv),
      detail::effective_rate(op.dr & 0x1F, ksv),
      detail::effective_rate(op.sr & 0x1F, ksv), sustain_attenuation(op.sl),
      eg_rate_hz(clock_hz));
  const double ramps_per_period = (op.ssg & 0x02) != 0 ? 2.0 : 1.0;
  return ramp * ramps_per_period;
}

} // namespace ym2612_eg
