#pragma once

// One-shot curve sampling on top of EgSimulator: a decimated polyline plus
// the event markers and warnings a musician-facing graph needs.

#include "simulator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ym2612_eg {

struct CurvePoint {
  float ms;
  uint16_t out; // after SSG inversion and TL, clamped
  uint16_t att; // internal attenuation
};

enum class MarkerKind : uint8_t {
  AttackEnd,
  DecayEnd,
  KeyOff,
  SsgFold,
  SsgInvert,
  SsgHold,
  SsgPhaseReset,
  Park,
  Silence,
};

struct Marker {
  float ms;
  MarkerKind kind;
};

enum class CurveWarning : uint8_t {
  SsgNeverLoops,
  SsgAudioRate,
  SsgArBelow31,
  AttackFrozen,
};

struct CurveRequest {
  OperatorParams op{};
  NotePitch pitch{};
  double gate_ms = -1.0; // key-held duration; < 0 means held forever
  double max_ms = 2000.0;
  double clock_hz = kNtscClockHz;
  uint16_t start_att = kMaxAttenuation; // retrigger support
};

struct CurveResult {
  std::vector<CurvePoint> points;
  std::vector<Marker> markers;
  double loop_hz = 0.0; // SSG loop frequency; 0 when not looping
  double park_ms = std::numeric_limits<double>::infinity();
  std::vector<CurveWarning> warnings;
};

namespace detail {

// Curve decimation budget.  RDP guarantees the decimated polyline stays
// within kDecimationEpsilon attenuation units of the undecimated one.
inline constexpr double kDecimationEpsilon = 0.9;
// A pathological patch (SSG alternate with AR < 31 toggles every sample) can
// raise an event on every single sample; cap what we record.
inline constexpr size_t kMaxMarkers = 4096;
/// Vertex ceiling. An envelope that oscillates at the sample rate -- an SSG-EG
/// alternate mode with AR < 31 -- has a genuine value on every sample, so
/// decimation cannot thin it and a ten-second span would carry half a million
/// vertices. Past this many, the curve is reduced to one bucket per slot
/// keeping that slot's extremes, which draws the oscillation as a band.
inline constexpr size_t kMaxPoints = 4096;
/// Teeth an SSG-EG alternate band is drawn with, counted over the axis it has
/// covered so far.  reduce() keeps the extremes of each of kMaxPoints / 2 time
/// slots, so four teeth to a slot leave every slot holding both levels.
inline constexpr uint64_t kAlternatingTeeth = 4 * (kMaxPoints / 2);

/// Reduce an already-decimated curve to at most kMaxPoints vertices, keeping
/// the loudest and quietest value of each time slot so the envelope's extent
/// survives even when its individual cycles cannot.
inline std::vector<CurvePoint> reduce(const std::vector<CurvePoint> &pts) {
  if (pts.size() <= kMaxPoints)
    return pts;
  const double t0 = pts.front().ms;
  const double span = static_cast<double>(pts.back().ms) - t0;
  const size_t slots = kMaxPoints / 2;
  std::vector<CurvePoint> out;
  out.reserve(kMaxPoints + 2);
  size_t i = 0;
  for (size_t slot = 0; slot < slots && i < pts.size(); ++slot) {
    const double end =
        span > 0.0 ? t0 + span * static_cast<double>(slot + 1) / slots
                   : std::numeric_limits<double>::infinity();
    size_t low = i, high = i;
    size_t n = 0;
    while (i < pts.size() &&
           (static_cast<double>(pts[i].ms) <= end || n == 0)) {
      if (pts[i].out < pts[low].out)
        low = i;
      if (pts[i].out > pts[high].out)
        high = i;
      ++i;
      ++n;
    }
    if (low == high) {
      out.push_back(pts[low]);
      continue;
    }
    out.push_back(pts[low < high ? low : high]);
    out.push_back(pts[low < high ? high : low]);
  }
  if (!out.empty() && out.back().ms != pts.back().ms)
    out.push_back(pts.back());
  return out;
}

// Ramer-Douglas-Peucker over one span, iterative so a 100k-point ramp cannot
// blow the stack.  Vertical distance, worst of the two channels.
inline void rdp_span(const std::vector<CurvePoint> &pts, size_t lo, size_t hi,
                     double eps, std::vector<uint8_t> &keep, uint64_t &budget) {
  std::vector<std::pair<size_t, size_t>> stack;
  stack.emplace_back(lo, hi);
  while (!stack.empty()) {
    if (budget == 0) {
      // Out of budget: keep everything still under consideration rather than
      // flattening it. Refining costs time; not refining costs vertices.
      for (const std::pair<size_t, size_t> &rest : stack)
        for (size_t i = rest.first; i <= rest.second && i < keep.size(); ++i)
          keep[i] = 1;
      return;
    }
    const std::pair<size_t, size_t> span = stack.back();
    stack.pop_back();
    const size_t a = span.first;
    const size_t b = span.second;
    if (b <= a + 1)
      continue;
    const double t0 = pts[a].ms;
    const double dt = static_cast<double>(pts[b].ms) - t0;
    double worst = -1.0;
    size_t worst_i = a;
    budget -= (budget < b - a) ? budget : (b - a);
    for (size_t i = a + 1; i < b; ++i) {
      const double u = dt > 0.0 ? (static_cast<double>(pts[i].ms) - t0) / dt : 0.0;
      const double po =
          pts[a].out + (static_cast<double>(pts[b].out) - pts[a].out) * u;
      const double pa =
          pts[a].att + (static_cast<double>(pts[b].att) - pts[a].att) * u;
      const double d = std::max(std::fabs(pts[i].out - po),
                                std::fabs(pts[i].att - pa));
      if (d > worst) {
        worst = d;
        worst_i = i;
      }
    }
    if (worst >= eps) {
      keep[worst_i] = 1;
      stack.emplace_back(a, worst_i);
      stack.emplace_back(worst_i, b);
    }
  }
}

/// Enough refinement for any curve a screen can show; a sample-rate
/// oscillation would otherwise make this quadratic.
inline constexpr uint64_t kDecimationBudget = 400000;

inline std::vector<CurvePoint> decimate(const std::vector<CurvePoint> &raw,
                                        std::vector<uint8_t> locked,
                                        double eps) {
  if (raw.size() < 3)
    return raw;
  locked.resize(raw.size(), 0);
  locked.front() = 1;
  locked.back() = 1;

  std::vector<uint8_t> keep = locked;
  uint64_t budget = kDecimationBudget;
  size_t anchor = 0;
  for (size_t i = 1; i < raw.size(); ++i) {
    if (!locked[i])
      continue;
    rdp_span(raw, anchor, i, eps, keep, budget);
    anchor = i;
  }

  std::vector<CurvePoint> out;
  out.reserve(raw.size() / 4 + 8);
  for (size_t i = 0; i < raw.size(); ++i)
    if (keep[i])
      out.push_back(raw[i]);
  return out;
}

} // namespace detail

inline CurveResult sample_curve(const CurveRequest &request) {
  CurveResult res;

  const double fs = sample_rate_hz(request.clock_hz);
  const double ms_per_sample = 1000.0 / fs;

  EgSimulator sim(request.op, request.pitch, request.clock_hz);
  sim.reset(0, request.start_att);
  sim.key_on();

  const bool ssg_enabled = (request.op.ssg & 0x08) != 0;
  const bool ssg_hold = (request.op.ssg & 0x01) != 0;
  const bool ssg_alternate = (request.op.ssg & 0x02) != 0;
  const bool looping_mode = ssg_enabled && !ssg_hold;
  // Modes 2 and 6 (alternate, no hold) draw two ramps per visible period.
  const int ramps_per_period = ssg_alternate ? 2 : 1;
  // Each fold ends one ramp, so five periods is 5 * ramps_per_period folds.
  const size_t fold_limit = static_cast<size_t>(5 * ramps_per_period);

  const bool gate_forever = request.gate_ms < 0.0;
  const double max_ms = request.max_ms > 0.0 ? request.max_ms : 0.0;
  const uint64_t max_samples =
      static_cast<uint64_t>(std::ceil(max_ms * fs / 1000.0));
  const uint64_t gate_sample =
      gate_forever ? ~uint64_t{0}
                   : static_cast<uint64_t>(std::llround(request.gate_ms * fs /
                                                        1000.0));

  std::vector<CurvePoint> raw;
  std::vector<uint8_t> locked;
  raw.reserve(1024);
  locked.reserve(1024);

  auto emit = [&](double ms, uint16_t o, uint16_t a, bool force) {
    const float fms = static_cast<float>(ms);
    if (!raw.empty() && raw.back().out == o && raw.back().att == a) {
      if (!force)
        return;
      if (raw.back().ms == fms) {
        locked.back() = 1;
        return;
      }
    }
    raw.push_back(CurvePoint{fms, o, a});
    locked.push_back(force ? uint8_t{1} : uint8_t{0});
  };

  auto push_point = [&](double ms, bool force) {
    emit(ms, sim.output(), sim.attenuation(), force);
  };

  auto add_marker = [&](double ms, MarkerKind kind) {
    if (res.markers.size() >= detail::kMaxMarkers)
      return false;
    res.markers.push_back(Marker{static_cast<float>(ms), kind});
    return true;
  };

  push_point(0.0, true);

  double silence_start_ms = sim.output() >= kSilenceAttenuation ? 0.0 : -1.0;
  size_t silence_index = 0;

  // The run into silence has to be unbroken, so any louder sample restarts it.
  auto track_silence = [&](double ms, uint16_t out) {
    if (out >= kSilenceAttenuation) {
      if (silence_start_ms < 0.0) {
        silence_start_ms = ms;
        silence_index = raw.size() - 1;
      }
    } else {
      silence_start_ms = -1.0;
    }
  };
  std::vector<uint64_t> fold_samples;
  bool key_off_done = gate_forever;
  bool parked = false;

  for (uint64_t i = 0; i < max_samples; ++i) {
    // Across a stretch the simulator cannot move through, every sample repeats
    // the last one: no event, no vertex the dedup would keep, no change of
    // silence or park state.  Cross it whole.  The two samples that are not
    // part of such a stretch are the one a still-unrecorded park lands on and
    // the one the key-off lands on.
    if (parked || !sim.is_static()) {
      uint64_t room = max_samples - i;
      if (!key_off_done)
        room = i < gate_sample ? std::min(room, gate_sample - i) : uint64_t{0};
      const uint64_t skip = std::min<uint64_t>(sim.skippable_samples(), room);
      if (skip >= 2) {
        sim.skip(static_cast<uint32_t>(skip));
        i += skip - 1;
        continue;
      }

      // The alternate fold squares the output between two levels at the sample
      // rate, so the picture is a band and a vertex per sample only redraws
      // its two edges.  Teeth spread over the axis so far carry the same band;
      // the stride is odd so that consecutive teeth keep alternating, and the
      // run's first and last samples are always drawn so the curve joins what
      // comes before and after unchanged.
      uint16_t first = 0, second = 0;
      const uint64_t band =
          std::min<uint64_t>(sim.alternating_samples(first, second), room);
      if (band >= 2) {
        uint64_t stride = (i + band) / detail::kAlternatingTeeth;
        // A run too short for that stride would come out as a single tooth,
        // and one tooth is a line rather than a band.
        const uint64_t own = band / 4;
        stride = (stride < own ? stride : own) | 1;
        const uint16_t att = sim.attenuation();
        for (uint64_t k = 0;; k += stride) {
          if (k > band - 1)
            k = band - 1;
          const double ms = static_cast<double>(i + k + 1) * 1000.0 / fs;
          const uint16_t out = (k & 1) ? second : first;
          emit(ms, out, att, add_marker(ms, MarkerKind::SsgInvert));
          track_silence(ms, out);
          if (k == band - 1)
            break;
        }
        sim.skip(static_cast<uint32_t>(band));
        i += band - 1;
        continue;
      }
    }

    if (!key_off_done && i >= gate_sample) {
      const double ms = static_cast<double>(i) * ms_per_sample;
      sim.key_off();
      add_marker(ms, MarkerKind::KeyOff);
      // Key-off can move the level on its own (the SSG inversion latch), so
      // record the new value at the key-off instant.
      push_point(ms, true);
      key_off_done = true;
      // Key-off puts the envelope in motion again (release), so re-arm the
      // park detector; without this a curve that parked during sustain hold
      // would end here and lose its whole release segment.
      parked = false;
      track_silence(ms, sim.output());
    }

    sim.step();
    const double ms = sim.time_ms();
    const uint32_t ev = sim.step_events();

    bool marked = false;
    if (ev != 0) {
      if (ev & detail::kEvAttackEnd)
        marked |= add_marker(ms, MarkerKind::AttackEnd);
      if (ev & detail::kEvDecayEnd)
        marked |= add_marker(ms, MarkerKind::DecayEnd);
      if (ev & detail::kEvSsgFold) {
        marked |= add_marker(ms, MarkerKind::SsgFold);
        fold_samples.push_back(i);
      }
      if (ev & detail::kEvSsgInvert)
        marked |= add_marker(ms, MarkerKind::SsgInvert);
      if (ev & detail::kEvSsgHold)
        marked |= add_marker(ms, MarkerKind::SsgHold);
      if (ev & detail::kEvSsgPhaseReset)
        marked |= add_marker(ms, MarkerKind::SsgPhaseReset);
    }
    push_point(ms, marked);
    track_silence(ms, sim.output());

    if (!parked && sim.is_static()) {
      parked = true;
      // park_ms is the FIRST time the envelope came to rest (e.g. the sustain
      // hold of an SR=0 patch); later Park markers still record re-parks.
      if (!std::isfinite(res.park_ms))
        res.park_ms = ms;
      add_marker(ms, MarkerKind::Park);
      push_point(ms, true);
    }
    if (parked && key_off_done)
      break;
    // Held-forever SSG loop: five full periods is all a graph needs.
    if (looping_mode && gate_forever && fold_samples.size() >= fold_limit)
      break;
  }

  // Close the polyline at the end of the simulated span.
  push_point(sim.time_ms(), true);

  // Loop frequency: mean of the fold-to-fold intervals, skipping the first
  // (it still carries the initial attack / start_att offset).
  if (looping_mode && fold_samples.size() >= 3) {
    double sum = 0.0;
    size_t n = 0;
    for (size_t k = 2; k < fold_samples.size(); ++k) {
      sum += static_cast<double>(fold_samples[k] - fold_samples[k - 1]);
      ++n;
    }
    const double ramp_samples = sum / static_cast<double>(n);
    const double period_s = ramp_samples * ramps_per_period / fs;
    if (period_s > 0.0)
      res.loop_hz = 1.0 / period_s;
  }

  // Silence: the moment the output went above the hardware mute floor and
  // stayed there.  Only meaningful once the envelope has come to rest.
  const bool at_rest = parked || sim.attenuation() >= kMaxAttenuation;
  if (at_rest && silence_start_ms >= 0.0) {
    add_marker(silence_start_ms, MarkerKind::Silence);
    if (silence_index < locked.size())
      locked[silence_index] = 1;
  }

  if (request.op.ar == 0)
    res.warnings.push_back(CurveWarning::AttackFrozen);
  if (ssg_enabled && request.op.ar < 31)
    res.warnings.push_back(CurveWarning::SsgArBelow31);
  if (looping_mode && ((request.op.sr == 0 && request.op.sl <= 14) ||
                       (request.op.dr == 0 && request.op.sl > 0)))
    res.warnings.push_back(CurveWarning::SsgNeverLoops);
  if (res.loop_hz > 20.0)
    res.warnings.push_back(CurveWarning::SsgAudioRate);

  std::stable_sort(res.markers.begin(), res.markers.end(),
                   [](const Marker &a, const Marker &b) { return a.ms < b.ms; });

  res.points = detail::reduce(
      detail::decimate(raw, std::move(locked), detail::kDecimationEpsilon));
  return res;
}

} // namespace ym2612_eg
