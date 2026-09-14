#pragma once

// Drawing policy over the simulator: one operator's registers turned into two
// millisecond-accurate traces on one elapsed-time axis, where a sounding voice
// sits on that axis, and how often either may be rebuilt. The two traces are
// independent: the held envelope is simulated with the key never released,
// the only way SR reads truthfully (SR = 0 holds flat, SR > 0 crawls); the
// release is simulated on its own from full volume. The solvers at the end
// run the other way, from a phase length -- or, for the sustain, the level the
// line has fallen to -- back to the register value that comes closest to it,
// which is what a dragged handle needs.

#include "detail/constants.hpp"
#include "detail/curve.hpp"
#include "detail/simulator.hpp"
#include "detail/timing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace ym2612_eg::graph {

/// Attenuation at the bottom of the graph; 0 (full volume) is at the top.
inline constexpr double kFullScale = static_cast<double>(kMaxAttenuation);

/// How wide the time axis may get, in ms: kMinSpanMs is the narrowest axis
/// worth drawing whatever the content says, kMinHeldMs the narrowest a loop is
/// given, kMaxHeldMs the widest an envelope that finishes is drawn at.
inline constexpr double kMinSpanMs = 25.0;
inline constexpr double kMinHeldMs = 50.0;
inline constexpr double kMaxHeldMs = 10000.0;
/// A sustain that never ends is a flat line; this much of the graph is enough
/// to say so.
inline constexpr double kFlatHoldShare = 0.05;
/// Nothing is drawn wider than this, not even an attack that outlasts it.
inline constexpr double kMaxSpanMs = 20000.0;

inline constexpr double kLoopVisiblePeriods = 1.2;
inline constexpr double kLoopMaxAxisMs = 20000.0;

/// Roughly this many SSG loop periods are drawn.
inline constexpr double kSsgLoopPeriods = 3.5;
/// How much of a looping graph the release may claim; past this the axis keeps
/// the loop's scale and the release runs off the right edge instead.
inline constexpr double kSsgSpanBudget = 2.0;
/// How long a release is simulated for. RR = 0 never reaches silence at all,
/// so there has to be a ceiling.
inline constexpr double kMaxReleaseMs = 10000.0;
/// How long a voice takes to fade from the graph once it is silent.
inline constexpr double kVoiceFadeMs = 400.0;

/// The first marker of `kind` on a trace, in ms, or negative when it has none.
inline double first_marker_ms(const CurveResult &curve, MarkerKind kind) {
  for (const Marker &m : curve.markers) {
    if (m.kind == kind) {
      return m.ms;
    }
  }
  return -1.0;
}

/// Whether two register sets draw the same curve: every field of the envelope,
/// so the caches rebuild on a change of any of them and on nothing else.
inline bool same_envelope(const OperatorParams &lhs, const OperatorParams &rhs) {
  return lhs.ar == rhs.ar && lhs.dr == rhs.dr && lhs.sr == rhs.sr &&
         lhs.rr == rhs.rr && lhs.sl == rhs.sl && lhs.tl == rhs.tl &&
         lhs.ks == rhs.ks && lhs.ssg == rhs.ssg;
}

/// The release's budget is its own: tying it to the held window would let AR
/// and DR change how long a release is drawn.
inline double release_max_ms() { return kMaxReleaseMs; }

/// The axis width the held envelope deserves: the end of its sustain, floored
/// at the instant the sustain begins so the attack and decay are never cut,
/// and bounded by kMaxSpanMs.
inline double window_for_timeline_ms(const PhaseDurations &phases) {
  const double sustain_start = phases.attack_ms + phases.decay_ms;
  // A sustain that never ends has no length to be the axis. It draws a flat
  // line, which says the same thing at any width, so it takes a fixed share of
  // the graph rather than a share of the axis it would otherwise decide.
  const double whole = std::isfinite(phases.sustain_ms)
                           ? sustain_start + phases.sustain_ms
                           : sustain_start / (1.0 - kFlatHoldShare);
  // The ceiling never cuts the attack and decay, and kMaxSpanMs is where that
  // concession runs out: a phase that never advances lasts forever, and
  // forever is not a width.
  const double floor_ms = std::clamp(sustain_start, kMinHeldMs, kMaxSpanMs);
  return std::clamp(whole, floor_ms, kMaxSpanMs);
}

namespace detail {

/// choose_held_ms() for an SSG-EG loop period already computed.
inline double held_ms_for_period(const OperatorParams &op, NotePitch pitch,
                                 double period) {
  // An infinite period is not a slow loop but no loop: the fold never comes,
  // and what the graph has to show is the phase that stalled.
  if (period > 0.0 && std::isfinite(period)) {
    const double held = kSsgLoopPeriods * period;
    // A loop carries its own time scale, so the periods are the floor; and the
    // ceiling gives way for a loop too slow to fit one period on it.
    const double ceiling = std::min(
        std::max(kMaxHeldMs, kLoopVisiblePeriods * period), kLoopMaxAxisMs);
    return std::clamp(held, std::min(kMinHeldMs, held), ceiling);
  }
  return window_for_timeline_ms(phase_durations(op, pitch));
}

} // namespace detail

/// How much of the held envelope is worth seeing at `pitch`, in ms: about
/// kSsgLoopPeriods periods of an SSG loop, or else window_for_timeline_ms() of
/// its phase durations.  A scale for the axis, not a key-off.
inline double choose_held_ms(const OperatorParams &op, NotePitch pitch) {
  return detail::held_ms_for_period(op, pitch, ssg_loop_period_ms(op, pitch));
}

namespace detail {

inline constexpr double kGridSteps[] = {5.0,    10.0,   25.0,   50.0,
                                        100.0,  250.0,  500.0,  1000.0,
                                        2500.0, 5000.0, 10000.0};

/// The Silence marker as a time, or infinity when the trace never gets there.
inline double silence_or_never(const CurveResult &curve) {
  const double ms = first_marker_ms(curve, MarkerKind::Silence);
  return ms >= 0.0 ? ms : std::numeric_limits<double>::infinity();
}

/// The slope the trace would be continued along past its last point, in
/// attenuation units per ms; zero when it came to rest or sits at one level.
inline double tail_slope(const CurveResult &curve, bool at_rest) {
  const auto &points = curve.points;
  if (at_rest || points.size() < 2) {
    return 0.0;
  }
  const CurvePoint &last = points.back();
  // sample_curve() closes every polyline at the end of the simulated span,
  // which can repeat the level already there -- a final edge both very short
  // and perfectly flat -- so measure from the last point at a different level.
  std::size_t base = points.size() - 1;
  while (base > 0 && points[base - 1].out == last.out) {
    --base;
  }
  if (base == 0) {
    return 0.0; // the whole trace sits at one level
  }
  const CurvePoint &previous = points[base - 1];
  const double dt = static_cast<double>(last.ms) - previous.ms;
  return dt > 0.0 ? (static_cast<double>(last.out) - previous.out) / dt : 0.0;
}

/// The mean rise of a phase that is linear in attenuation, in units per ms.
/// Every tick adds the same mean increment, so a trace that stops inside such
/// a phase goes on at this slope -- read off the trace instead, the last edge
/// of a slow phase can be a single step of its staircase.
inline double linear_phase_slope(int rate) {
  return ym2612_eg::detail::atten_per_eg_tick(rate, false) *
         eg_rate_hz(kNtscClockHz) / 1000.0;
}

} // namespace detail

/// The grid/label interval for a given span: a round number, 3-6 divisions.
inline double grid_step_ms(double span_ms) {
  for (const double step : detail::kGridSteps) {
    if (span_ms <= step * 6.0) {
      return step;
    }
  }
  return detail::kGridSteps[std::size(detail::kGridSteps) - 1];
}

/// The warning worth showing, or empty: how often the sustain level is skipped
/// where the key-on decides it, else an SSG-EG mode on an attack rate below the
/// 31 the hardware convention expects. The rest shows in the curve's shape.
inline std::string warning_line(const CurveResult &curve,
                                double sl_skip_probability = 0.0) {
  if (sl_skip_probability > 0.0 && sl_skip_probability < 1.0) {
    return "SL skipped on " +
           std::to_string(std::lround(sl_skip_probability * 100.0)) +
           "% of notes";
  }
  for (const CurveWarning w : curve.warnings) {
    if (w == CurveWarning::SsgArBelow31) {
      return "AR<31: non-standard SSG-EG";
    }
  }
  return {};
}

/// Everything the graph needs for one operator.
struct EnvelopeCurve {
  /// Attack, decay and sustain with the key never released: drawn as a line.
  CurveResult held;
  /// A release from full volume, starting at t = 0: drawn as a filled area.
  CurveResult release;

  /// The width of the time axis the curve was simulated for.
  double span_ms = 0.0;
  double held_ms = 0.0; ///< the window the axis width was chosen from
  /// Where the release polyline ends: silence, unless its budget ran out
  /// first. The axis is sized from it.
  double release_content_ms = 0.0;

  /// The held envelope came to rest, so simulating it further would only
  /// repeat one level.
  bool held_parked = false;
  /// The release was still falling when its budget ran out.
  bool release_truncated = false;

  /// The slope, in attenuation units per ms, a trace that stops before the
  /// right-hand edge is continued along -- zero when it came to rest or sits
  /// at one level. Exact: every post-attack segment is linear in attenuation.
  double held_tail_slope = 0.0;
  double release_tail_slope = 0.0;

  /// The first instant each trace is at or below the hardware mute floor and
  /// stays there. Infinite when the trace never gets there.
  double held_silence_ms = std::numeric_limits<double>::infinity();
  double release_silence_ms = std::numeric_limits<double>::infinity();

  // Segment boundaries on the held trace, ms; negative when the segment never
  // happened.
  double attack_end_ms = -1.0;
  double decay_end_ms = -1.0;

  /// Every instant the held trace folds, in order -- an SSG-EG loop's teeth.
  std::vector<float> ssg_folds;

  uint16_t peak_out = 0;    ///< output attenuation at full volume (TL * 8)
  uint16_t sustain_out = 0; ///< output attenuation the decay aims at (SL + TL)

  /// The fraction of key-on phases whose first decay steps past the sustain
  /// window; strictly between 0 and 1 only where the key-on decides it.
  double sl_skip_probability = 0.0;
  std::string warning; ///< empty when there is none
};

/// One operator's curves at `pitch`: a release from full volume and the held
/// trace on one time axis. The held trace covers at least `min_span_ms`.
inline EnvelopeCurve build_envelope_curve(const OperatorParams &op,
                                          NotePitch pitch,
                                          double min_span_ms = 0.0) {
  EnvelopeCurve out;

  // 1. What the axis has to hold: the loop period, or the phase durations.
  const double period_ms = ssg_loop_period_ms(op, pitch);
  const bool loops = period_ms > 0.0 && std::isfinite(period_ms);
  out.held_ms = detail::held_ms_for_period(op, pitch, period_ms);

  // 2. The release on its own, keyed on at full volume and released at once,
  //    so it takes the chip's key-off rules: the SSG inversion latch, the 4x
  //    increments, the hard cut at 0x200.
  CurveRequest release;
  release.op = op;
  // The key-on sample would snap an instant attack to att = 0, and the
  // release rate does not depend on AR.
  release.op.ar = 0;
  release.pitch = pitch;
  // The key comes up two samples in; released on the sample it went down, it
  // never starts.
  release.gate_ms = 2000.0 / sample_rate_hz(kNtscClockHz);
  release.max_ms = release_max_ms();
  // Full volume is one step short of loudest_attenuation(): for an inverted
  // SSG-EG mode that is the fold level, which an operator only passes through.
  const uint16_t loudest = loudest_attenuation(op);
  release.start_att = loudest > 0 ? static_cast<uint16_t>(loudest - 1) : 0;
  out.release = sample_curve(release);
  // The release is drawn from the end of the key-on sample: points before it
  // are dropped and the first at or after it becomes the origin.
  {
    const float settled =
        static_cast<float>(1000.0 / sample_rate_hz(kNtscClockHz));
    std::vector<CurvePoint> &points = out.release.points;
    const auto first =
        std::find_if(points.begin(), points.end(),
                     [settled](const CurvePoint &p) { return p.ms >= settled; });
    if (first != points.begin() && first != points.end()) {
      points.erase(points.begin(), first);
      const float origin = points.front().ms;
      for (CurvePoint &p : points)
        p.ms -= origin;
      for (Marker &m : out.release.markers)
        m.ms = m.ms > origin ? m.ms - origin : 0.0f;
    }
  }
  out.release_content_ms =
      out.release.points.empty()
          ? 0.0
          : static_cast<double>(out.release.points.back().ms);
  // A release ends where the chip cuts the output dead, which is a level of
  // ATTENUATION rather than one on the graph: TL lifts the whole envelope, so
  // the drawn line can be at the bottom of the scale while the attenuation
  // behind it still has ground to cover. Short of the cut, the run stopped
  // because its budget did.
  out.release_truncated = !out.release.points.empty() &&
                          out.release.points.back().att < kCutAttenuation;

  // 3. The axis has to hold both traces -- but neither may crush the other. A
  //    loop keeps its own scale, and a release much longer than the held
  //    envelope is allowed to run off the right edge rather than flatten the
  //    part being edited. The width is the content itself, at full precision.
  double content = std::max(out.held_ms, out.release_content_ms);
  if (loops && out.held_ms > 0.0) {
    content = std::min(content, out.held_ms * kSsgSpanBudget);
  }
  out.span_ms = std::max(content, kMinSpanMs);

  // 4. The held trace, simulated across the WHOLE axis rather than only as far
  //    as the window policy asked for. Still no key-off, so SR = 0 holds flat
  //    and SR > 0 shows its real decay. gate_ms < 0 means more to
  //    sample_curve() than "never released": it also stops early once a loop
  //    has repeated five periods, leaving the trace in mid-air -- so a loop
  //    gets a key-off just past the end of the window instead, off the graph.
  CurveRequest request;
  request.op = op;
  request.pitch = pitch;
  // A voice overlay is drawn on another curve's axis, and a loop has to be
  // simulated across it rather than extrapolated along its last ramp.
  request.max_ms = std::max({out.span_ms, out.held_ms, min_span_ms});
  request.gate_ms = loops ? request.max_ms + 1.0 : -1.0;
  out.held = sample_curve(request);
  // A finite park is exactly "the trace ended because there was nothing left
  // to draw" -- continue it flat.
  out.held_parked = std::isfinite(out.held.park_ms);

  out.attack_end_ms = first_marker_ms(out.held, MarkerKind::AttackEnd);
  out.decay_end_ms = first_marker_ms(out.held, MarkerKind::DecayEnd);

  // The markers come back sorted by time, so the folds do too.
  for (const Marker &m : out.held.markers) {
    if (m.kind == MarkerKind::SsgFold) {
      out.ssg_folds.push_back(m.ms);
    }
  }

  // Silence is only raised once the trace has come to rest.
  out.held_silence_ms = detail::silence_or_never(out.held);
  out.release_silence_ms = detail::silence_or_never(out.release);

  // A trace that stops inside a linear phase goes on at that phase's rate. An
  // attack still under way keeps the slope of its last edge, since it is not
  // linear, and so does an SSG-EG trace, which folds.
  const bool ssg = (op.ssg & 0x08) != 0;
  const int ksv = key_scale_value(op, pitch);
  out.held_tail_slope = detail::tail_slope(out.held, out.held_parked);
  if (!out.held_parked && !ssg && out.attack_end_ms >= 0.0) {
    const int rate = out.decay_end_ms >= 0.0 ? op.sr : op.dr;
    out.held_tail_slope = detail::linear_phase_slope(
        ym2612_eg::detail::effective_rate(rate & 0x1F, ksv));
  }
  out.release_tail_slope =
      detail::tail_slope(out.release, !out.release_truncated);
  if (out.release_truncated && !ssg) {
    out.release_tail_slope = detail::linear_phase_slope(
        ym2612_eg::detail::effective_rate(2 * (op.rr & 0x0F) + 1, ksv));
  }

  const int tl_att = static_cast<int>(op.tl) * 8;
  out.peak_out =
      static_cast<uint16_t>(std::min(tl_att, static_cast<int>(kMaxAttenuation)));
  // The level the decay aims at, in the same output units as the curve.
  out.sustain_out = static_cast<uint16_t>(
      std::min(sustain_attenuation(op.sl) + tl_att,
               static_cast<int>(kMaxAttenuation)));

  out.sl_skip_probability = sl_skip_probability(op, pitch);
  out.warning = warning_line(out.held, out.sl_skip_probability);
  return out;
}

// ------------------------------------------------------- the drawn traces

/// One vertex of a trace as it is drawn: where on the graph's time axis, in
/// ms, and the output attenuation there.
struct TraceVertex {
  double ms = 0.0;
  double out = 0.0;
};

/// `trace` as the graph draws it, written into `path`: entered at `from_ms` on
/// the trace, slid `shift_ms` along the axis, and ended at `limit_ms` on the
/// axis or at `span_ms`, whichever is first. Past its last point the trace
/// goes on along `tail_slope` until it meets the top or bottom of the scale.
/// An edge that crosses either end is cut there.
inline void build_trace_path(
    std::vector<TraceVertex> &path, const CurveResult &trace, double tail_slope,
    double span_ms, double from_ms = 0.0,
    double limit_ms = std::numeric_limits<double>::infinity(),
    double shift_ms = 0.0) {
  path.clear();
  const std::vector<CurvePoint> &points = trace.points;
  if (points.empty()) {
    return;
  }
  // In the trace's own time until a vertex is written.
  const double limit = std::min(limit_ms, span_ms) - shift_ms;
  if (!(limit > from_ms)) {
    return;
  }

  // One more edge past the last point, stopped where the slope meets the top
  // or the bottom of the scale.
  const CurvePoint &last = points.back();
  double tail_ms = limit;
  double tail_out = last.out;
  bool has_tail = last.ms < limit;
  if (has_tail && tail_slope > 0.0) {
    tail_ms = std::min(tail_ms, last.ms + (kFullScale - last.out) / tail_slope);
  } else if (has_tail && tail_slope < 0.0) {
    tail_ms = std::min(tail_ms, last.ms + (0.0 - last.out) / tail_slope);
  }
  if (has_tail) {
    tail_out = std::clamp(last.out + tail_slope * (tail_ms - last.ms), 0.0,
                          kFullScale);
    has_tail = tail_ms > last.ms;
  }

  const std::size_t edges = points.size() - 1 + (has_tail ? 1 : 0);
  path.reserve(edges + 1);
  for (std::size_t i = 0; i < edges; ++i) {
    double ms0 = points[i].ms;
    double out0 = points[i].out;
    double ms1 = tail_ms;
    double out1 = tail_out;
    if (i + 1 < points.size()) {
      ms1 = points[i + 1].ms;
      out1 = points[i + 1].out;
    }
    if (ms1 <= from_ms) {
      continue;
    }
    if (ms0 >= limit) {
      break;
    }
    if (ms0 < from_ms) {
      const double dt = ms1 - ms0;
      const double t = dt > 0.0 ? (from_ms - ms0) / dt : 0.0;
      out0 = out0 + (out1 - out0) * t;
      ms0 = from_ms;
    }
    if (ms1 > limit) {
      const double dt = ms1 - ms0;
      const double t = dt > 0.0 ? (limit - ms0) / dt : 0.0;
      out1 = out0 + (out1 - out0) * t;
      ms1 = limit;
    }
    if (path.empty()) {
      path.push_back(TraceVertex{ms0 + shift_ms, out0});
    }
    path.push_back(TraceVertex{ms1 + shift_ms, out1});
    if (ms1 >= limit) {
      break;
    }
  }
}

/// Which register's phase the held trace is in at `ms`. A phase that never
/// ends runs on, and with no key-off on the trace the sustain owns everything
/// past the decay.
inline EgPhase held_phase_at(const EnvelopeCurve &curve, double ms) {
  constexpr double kNever = std::numeric_limits<double>::infinity();
  const double attack_end =
      curve.attack_end_ms >= 0.0 ? curve.attack_end_ms : kNever;
  const double decay_end = std::max(
      curve.decay_end_ms >= 0.0 ? curve.decay_end_ms : kNever, attack_end);
  if (ms < attack_end) {
    return EgPhase::Attack;
  }
  if (ms < decay_end) {
    return EgPhase::Decay;
  }
  return EgPhase::Sustain;
}

/// A stretch of the held line one phase owns: `count` vertices from `first`,
/// the last of them shared with the next run.
struct PhaseRun {
  EgPhase phase = EgPhase::Attack;
  std::size_t first = 0;
  std::size_t count = 0;
};

struct PhaseRuns {
  std::array<PhaseRun, 3> items{};
  int count = 0;
};

/// The held trace's `path`, built with no shift, cut where it changes phase:
/// each edge belongs to the phase at the vertex it starts from.
inline PhaseRuns held_phase_runs(const EnvelopeCurve &curve,
                                 const std::vector<TraceVertex> &path) {
  PhaseRuns runs;
  const std::size_t edges = path.size() < 2 ? 0 : path.size() - 1;
  std::size_t start = 0;
  while (start < edges) {
    const EgPhase phase = held_phase_at(curve, path[start].ms);
    std::size_t end = start + 1;
    // An edge never goes back to an earlier phase, so there are three runs at
    // most.
    while (end < edges && held_phase_at(curve, path[end].ms) <= phase) {
      ++end;
    }
    runs.items[static_cast<std::size_t>(runs.count++)] =
        PhaseRun{phase, start, end - start + 1};
    start = end;
  }
  return runs;
}

// ------------------------------------------------------- the live cursor

/// The polyline's internal attenuation at `ms`, linearly interpolated and
/// clamped to the ends.
inline double curve_out_at_ms(const CurveResult &curve, double ms) {
  const auto &points = curve.points;
  if (points.empty()) {
    return static_cast<double>(kMaxAttenuation);
  }
  if (!(ms > points.front().ms)) {
    return points.front().out;
  }
  if (ms >= points.back().ms) {
    return points.back().out;
  }
  // Binary search rather than a walk: an SSG trace reduced to one bucket per
  // slot carries thousands of points, and this runs once per voice per frame.
  std::size_t lo = 0;
  std::size_t hi = points.size() - 1;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (static_cast<double>(points[mid].ms) <= ms) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double t0 = points[lo].ms;
  const double t1 = points[hi].ms;
  const double dt = t1 - t0;
  if (!(dt > 0.0)) {
    return points[hi].out;
  }
  const double u = (ms - t0) / dt;
  return points[lo].out +
         (static_cast<double>(points[hi].out) - points[lo].out) * u;
}

/// The first instant in [from_ms, to_ms] at which `trace` is drawn at or past
/// `level`, or `to_ms` if it never is. Each phase is monotone and RDP leaves a
/// straight edge between vertices, so interpolating inside the crossing edge
/// is exact. This is where the drawn line arrives at a level the registers put
/// somewhere else: TL lifts the whole envelope, and the output saturates
/// before the attenuation does.
inline double first_time_at_level(const CurveResult &trace, double level,
                                  double from_ms, double to_ms) {
  const auto &points = trace.points;
  if (points.empty() || from_ms >= to_ms) {
    return to_ms;
  }
  if (curve_out_at_ms(trace, from_ms) >= level) {
    return from_ms;
  }
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double ms0 = points[i - 1].ms;
    const double ms1 = points[i].ms;
    if (ms1 <= from_ms) {
      continue;
    }
    if (ms0 >= to_ms) {
      break;
    }
    const double a0 = points[i - 1].out;
    const double a1 = points[i].out;
    if (a1 < level) {
      continue;
    }
    double at = ms1; // a step, not a ramp: it arrives at this instant
    if (a1 > a0) {
      const double u = std::clamp((level - a0) / (a1 - a0), 0.0, 1.0);
      at = ms0 + (ms1 - ms0) * u;
    }
    return std::clamp(at, from_ms, to_ms);
  }
  return to_ms;
}

/// Where a voice let go at `out` joins the release trace. The release is
/// linear in attenuation, so a note let go at level L follows precisely the
/// trace already on screen, entered later -- the first instant the trace
/// reaches `out` IS where the voice joins it. Takes the trace rather than the
/// whole curve, so the same question can be put to a release the simulator
/// really ran.
inline double release_entry_ms(const CurveResult &release, double out) {
  if (release.points.empty()) {
    return 0.0;
  }
  return first_time_at_level(release, out, release.points.front().ms,
                             release.points.back().ms);
}

/// Where a sounding voice is on its own envelope.
struct VoiceCursor {
  /// x on the graph's time axis, in ms; past the end of the axis for a voice
  /// that has outrun it, which is not drawn rather than pinned to the edge.
  double ms = 0.0;
  /// The voice is past key-off and riding the release trace.
  bool released = false;
  /// How long the voice has been inaudible; 0 while it can still be heard.
  double silent_for_ms = 0.0;
  /// Where on the release trace this voice's release begins -- the point at
  /// which the drawn release is already at the level the key came up on.
  /// Negative while the key is still down.
  double release_from_ms = -1.0;
  /// Where on the graph that release is drawn from: the instant the key came
  /// up, on the held trace's own time axis. Negative while held.
  double release_origin_ms = -1.0;
  /// How much of the held trace this voice has actually been through. It only
  /// ever grows, and stops growing at key-off.
  double held_to_ms = 0.0;
};

/// Fold the elapsed time into the loop the axis draws. Returns `elapsed_ms`
/// unchanged unless the held trace loops and the cursor has run past the last
/// whole period on the axis. The first fold anchors the phase: everything
/// before it is the attack the loop only performs once.
inline double wrapped_into_loop_ms(const EnvelopeCurve &curve, double elapsed_ms,
                                   double axis_span_ms) {
  if (!(curve.held.loop_hz > 0.0) || !(axis_span_ms > 0.0) ||
      curve.ssg_folds.empty()) {
    return elapsed_ms;
  }
  // The folds are in order, so the last one on the axis is a search.
  const auto past_edge = std::upper_bound(
      curve.ssg_folds.begin(), curve.ssg_folds.end(), axis_span_ms,
      [](double edge, float fold) { return edge < fold; });
  if (past_edge == curve.ssg_folds.begin()) {
    return elapsed_ms; // not even the first fold is on the axis
  }
  const double first_fold = curve.ssg_folds.front();
  const double last_fold = *(past_edge - 1);
  // Two folds bound at least one whole period; one bounds none.
  const double window = last_fold - first_fold;
  if (!(window > 0.0) || elapsed_ms <= last_fold) {
    return elapsed_ms;
  }
  return first_fold + std::fmod(elapsed_ms - first_fold, window);
}

/// The cursor for one voice, given how long ago its key went down and (if it
/// has) come up. `since_key_off_ms` is negative while the key is still held.
///   - the cursor advances only while the envelope is CHANGING, so an SR = 0
///     patch's cursor waits at the park rather than sliding along a flat line;
///   - on key-off it moves to the release trace, at the point where that
///     trace is already at the level the voice actually had, and advances
///     from there.
inline VoiceCursor cursor_for_voice(const EnvelopeCurve &curve,
                                    double since_key_on_ms,
                                    double since_key_off_ms,
                                    double axis_span_ms) {
  VoiceCursor cursor;
  cursor.released = since_key_off_ms >= 0.0;

  // Where the held trace comes to rest, if it does. Past this instant nothing
  // about the envelope changes, so neither does the cursor -- an SR = 0 patch
  // parks at its sustain level and waits for the key to come up.
  const double park_ms = curve.held.park_ms;
  const double held_ms = std::max(since_key_on_ms, 0.0);

  double on_trace_ms = 0.0;
  double silence_ms = 0.0;
  if (!cursor.released) {
    on_trace_ms = std::min(held_ms, park_ms);
    // How far the note has actually been: measured before the wrap folds a
    // loop's elapsed time back onto the axis, so a loop past its first pass
    // has been through the whole of it.
    cursor.held_to_ms = on_trace_ms;
    silence_ms = curve.held_silence_ms;
  } else {
    const double released_for = std::max(since_key_off_ms, 0.0);
    // The level the voice actually had when the key came up -- park-clamped
    // for the same reason as above.
    const double at_key_off_ms =
        std::min(std::max(held_ms - released_for, 0.0), park_ms);
    // The AUDIBLE level, not the internal one: on key-off an inverted SSG-EG
    // mode latches what was being heard into the attenuation, so a release
    // continues from the level the ear was on.
    const double out = curve_out_at_ms(curve.held, at_key_off_ms);
    // The release from that level is not a new curve: it is the drawn one,
    // entered at the point where it is already at that level.
    cursor.release_from_ms = release_entry_ms(curve.release, out);
    cursor.release_origin_ms = at_key_off_ms;
    // The held part stops growing the moment the key comes up, however long
    // the release runs on after it.
    cursor.held_to_ms = at_key_off_ms;
    // The release is drawn from where the note actually let go, so the cursor
    // carries on from where it is rather than jumping to wherever that level
    // sits on a release that began at full volume.
    on_trace_ms = at_key_off_ms + released_for;
    // Silence is a property of the release, so it moves with it.
    silence_ms = std::isfinite(curve.release_silence_ms)
                     ? at_key_off_ms + curve.release_silence_ms -
                           cursor.release_from_ms
                     : curve.release_silence_ms;
  }

  if (!cursor.released) {
    // A loop never parks, so the cursor wraps with it instead of stopping at
    // the right-hand edge while the sound carries on.
    on_trace_ms = wrapped_into_loop_ms(curve, on_trace_ms, axis_span_ms);
  }

  // Measured before the axis clamp, so a voice whose cursor is parked at the
  // right-hand edge still fades out when the envelope beneath it dies.
  cursor.silent_for_ms =
      std::isfinite(silence_ms) ? std::max(0.0, on_trace_ms - silence_ms) : 0.0;
  // Still moving when it reaches the right-hand end of the axis: it carries on
  // past it and stops being drawn. Parking it on the edge would say the
  // envelope had come to rest there, which it has not.
  cursor.ms = std::max(on_trace_ms, 0.0);
  cursor.held_to_ms =
      std::clamp(cursor.held_to_ms, 0.0, std::max(axis_span_ms, 0.0));
  return cursor;
}

/// How much of a voice a graph still shows.
struct VoiceVisibility {
  /// 1 while the voice can be heard, falling to 0 over the fade time once it
  /// is silent.
  double fade = 1.0;
  /// Released and silent for the whole fade time: nothing of it shows again.
  bool finished = false;
  /// The cursor is on the axis. Past the right-hand edge only the cursor
  /// goes; the voice's curve and release stay until it has faded.
  bool cursor_on_axis = false;
};

/// `span_ms` is the axis actually drawn, which need not be the curve's own.
inline VoiceVisibility voice_visibility(const VoiceCursor &cursor,
                                        double span_ms,
                                        double fade_ms = kVoiceFadeMs) {
  VoiceVisibility out;
  const double silent = cursor.silent_for_ms;
  out.fade =
      silent > 0.0 ? std::clamp(1.0 - silent / fade_ms, 0.0, 1.0) : 1.0;
  out.finished = cursor.released && silent > 0.0 && silent >= fade_ms;
  out.cursor_on_axis = cursor.ms >= 0.0 && cursor.ms <= span_ms;
  return out;
}

/// Whether a voice let go `since_key_off_ms` ago (negative while held) has
/// nothing left on any graph: a release is simulated for release_max_ms() at
/// most, and its fade is over `fade_ms` after that.
inline bool voice_expired(double since_key_off_ms,
                          double fade_ms = kVoiceFadeMs) {
  return since_key_off_ms > release_max_ms() + fade_ms;
}

/// The curves of the notes being played, keyed on (registers, ksv) rather
/// than on the note: an operator's envelope depends on the note ONLY through
/// `ksv = keycode >> (3 - KS)`, so with KS = 0 the whole keyboard has four
/// distinct entries and a chord inside one octave shares a single one. At
/// most six entries, because at most six voices can sound; the least
/// recently asked-for is evicted. Entries are held by value in a fixed array,
/// so a reference handed out stays valid across later get() calls.
class VoiceCurveCache {
public:
  static constexpr std::size_t kMaxEntries = 6;

  /// The curve for `op` at `pitch`, drawn on `reference`'s axis, where
  /// `reference` is the curve of the same registers at `reference_pitch`.
  /// Returns `reference` itself when the two share a key-scale value, or a
  /// cached entry when there is one -- neither costs a simulation. The whole
  /// cache is dropped when the registers or the reference change. Building a
  /// curve costs one unit of `build_budget`; with none left this returns
  /// nullptr and the caller leaves that voice for the next frame.
  const EnvelopeCurve *get(const OperatorParams &op, NotePitch pitch,
                           const EnvelopeCurve &reference,
                           NotePitch reference_pitch, int &build_budget);

  /// Curves actually simulated, and entries held.
  int rebuild_count() const { return rebuilds_; }
  std::size_t size() const { return used_; }

private:
  struct Entry {
    int ksv = -1;
    uint64_t last_used = 0;
    EnvelopeCurve curve;
  };

  OperatorParams params_{};
  NotePitch reference_pitch_{};
  double reference_span_ms_ = 0.0;
  bool valid_ = false;
  std::array<Entry, kMaxEntries> entries_{};
  std::size_t used_ = 0;
  uint64_t clock_ = 0;
  int rebuilds_ = 0;
};

inline const EnvelopeCurve *VoiceCurveCache::get(const OperatorParams &op,
                                                 NotePitch pitch,
                                                 const EnvelopeCurve &reference,
                                                 NotePitch reference_pitch,
                                                 int &build_budget) {
  const bool same_context = valid_ && same_envelope(op, params_) &&
                            reference_pitch.fnum == reference_pitch_.fnum &&
                            reference_pitch.block == reference_pitch_.block &&
                            reference_span_ms_ == reference.span_ms;
  if (!same_context) {
    // Everything here was built for registers or an axis that no longer
    // exist, so the lot goes.
    entries_ = {};
    used_ = 0;
    params_ = op;
    reference_pitch_ = reference_pitch;
    reference_span_ms_ = reference.span_ms;
    valid_ = true;
  }

  const int ksv = key_scale_value(op, pitch);
  // The note reaches the envelope only through ksv, so a voice that shares the
  // reference note's is drawn by the curve already on screen. With KS = 0 that
  // is a whole two octaves either side of the reference.
  if (ksv == key_scale_value(op, reference_pitch)) {
    return &reference;
  }

  ++clock_;
  for (std::size_t i = 0; i < used_; ++i) {
    if (entries_[i].ksv == ksv) {
      entries_[i].last_used = clock_;
      return &entries_[i].curve;
    }
  }

  if (build_budget <= 0) {
    return nullptr; // next frame
  }
  --build_budget;

  std::size_t slot = used_;
  if (used_ < kMaxEntries) {
    ++used_;
  } else {
    // Six voices, six entries: this only fires when the sounding notes have
    // moved on, and then the entry going out is one nothing is playing.
    slot = 0;
    for (std::size_t i = 1; i < used_; ++i) {
      if (entries_[i].last_used < entries_[slot].last_used) {
        slot = i;
      }
    }
  }
  entries_[slot].ksv = ksv;
  entries_[slot].last_used = clock_;
  entries_[slot].curve = build_envelope_curve(op, pitch, reference.span_ms);
  ++rebuilds_;
  return &entries_[slot].curve;
}

// ------------------------------------------------- how often to rebuild

/// How much of a frame one operator's graph may spend rebuilding its curve,
/// and the frame that budget is per: enough that an ordinary patch is never
/// throttled, and little enough to bound four SSG-EG patches dragged at once.
inline constexpr double kRebuildBudgetMs = 1.5;
inline constexpr double kRebuildBudgetPeriodMs = 1000.0 / 60.0;
/// ... and the longest the graph may lag the registers however expensive the
/// curve is: below this the throttle just makes a drag feel less smooth,
/// above it the graph would look frozen rather than slow.
inline constexpr double kMaxRebuildDeferMs = 150.0;

/// Whether a rebuild is allowed to happen yet, from what the last one cost: a
/// curve that takes k times the budget is spaced k frames apart, so every
/// patch spends the same share of the machine however slow it is to
/// simulate. Nothing here knows about a frame, only the time and the cost.
class RebuildThrottle {
public:
  /// How long a rebuild costing `cost_ms` earns itself before the next one.
  static double interval_for_ms(double cost_ms);

  /// Whether a rebuild may run at `now_ms`. True until one has been recorded:
  /// the first curve is never deferred, because there is nothing to draw
  /// instead of it.
  bool may_rebuild(double now_ms) const;

  /// One rebuild, at `now_ms`, that took `cost_ms`.
  void note_rebuild(double now_ms, double cost_ms);

  double last_cost_ms() const { return last_cost_ms_; }
  double interval_ms() const { return interval_for_ms(last_cost_ms_); }

private:
  double last_rebuild_ms_ = 0.0;
  double last_cost_ms_ = 0.0;
  bool ever_ = false;
};

inline double RebuildThrottle::interval_for_ms(double cost_ms) {
  if (!(cost_ms > 0.0)) {
    return 0.0;
  }
  // The budget is a share of wall-clock time, so the interval is the cost
  // divided by that share: a rebuild worth one budget's work every frame is
  // spaced one frame apart, and one worth six is spaced six.
  const double interval = cost_ms * (kRebuildBudgetPeriodMs / kRebuildBudgetMs);
  return std::min(interval, kMaxRebuildDeferMs);
}

inline bool RebuildThrottle::may_rebuild(double now_ms) const {
  if (!ever_) {
    return true;
  }
  return now_ms - last_rebuild_ms_ >= interval_ms();
}

inline void RebuildThrottle::note_rebuild(double now_ms, double cost_ms) {
  last_rebuild_ms_ = now_ms;
  // A clock that went backwards, or a rebuild too quick to measure, is worth
  // nothing to the spacing: it earns no wait at all.
  last_cost_ms_ = std::max(cost_ms, 0.0);
  ever_ = true;
}

/// A monotonic clock in milliseconds -- what the throttle runs on unless a
/// test hands it another one. The throttle measures work rather than frames,
/// so it is wall-clock rather than the caller's frame counter.
inline double steady_now_ms() {
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration<double, std::milli>(since_epoch).count();
}

/// Remembers one operator's curve and rebuilds it only when the registers (or
/// the note) actually change. While dragging, a rebuild the throttle has not
/// licensed yet is skipped and last frame's curve handed back instead --
/// except the value a drag ends on is always built the frame after it stops
/// moving, whatever the throttle says.
class EnvelopeCurveCache {
public:
  const EnvelopeCurve &get(const OperatorParams &op, NotePitch pitch);

  /// The clock the throttle runs on. A rebuild's cost is the difference
  /// between a read taken when the cache is asked and one taken immediately
  /// afterwards, so replacing it is the only way to say what a rebuild costs.
  using Clock = double (*)();
  void set_clock(Clock clock) { now_ms_ = clock; }

  /// Curves actually simulated; see VoiceCurveCache::rebuild_count().
  int rebuild_count() const { return rebuilds_; }

private:
  OperatorParams params_{};
  NotePitch pitch_{};
  /// What the previous frame asked for, which is not always what was built:
  /// a request that repeats is a value that has settled.
  OperatorParams requested_{};
  NotePitch requested_pitch_{};
  bool requested_valid_ = false;
  EnvelopeCurve curve_;
  bool valid_ = false;
  int rebuilds_ = 0;
  RebuildThrottle throttle_;
  Clock now_ms_ = &steady_now_ms;
};

inline const EnvelopeCurve &EnvelopeCurveCache::get(const OperatorParams &op,
                                                    NotePitch pitch) {
  const auto same_pitch = [](NotePitch lhs, NotePitch rhs) {
    return lhs.fnum == rhs.fnum && lhs.block == rhs.block;
  };

  // Whether this is the second frame in a row to ask for the same thing --
  // measured against the previous REQUEST rather than against the curve, so a
  // value that arrived while a rebuild was deferred still counts as settled.
  const bool settled = requested_valid_ && same_envelope(op, requested_) &&
                       same_pitch(pitch, requested_pitch_);
  requested_ = op;
  requested_pitch_ = pitch;
  requested_valid_ = true;

  if (valid_ && same_envelope(op, params_) && same_pitch(pitch, pitch_)) {
    // The curve on hand is the one being asked for: nothing to decide, and no
    // clock read either. This is every frame the user is not editing.
    return curve_;
  }

  const double now_ms = now_ms_();
  if (valid_ && !settled && !throttle_.may_rebuild(now_ms)) {
    // Still moving, and the last rebuild has not earned its keep yet. Last
    // frame's curve goes out again -- one frame further out of date than the
    // one the graph drew a moment ago, which is a difference no drag can see.
    return curve_;
  }

  curve_ = build_envelope_curve(op, pitch);
  params_ = op;
  pitch_ = pitch;
  valid_ = true;
  ++rebuilds_;
  throttle_.note_rebuild(now_ms, now_ms_() - now_ms);
  return curve_;
}

// ------------------------------------------------- from a shape to a value

namespace detail {

/// Half an EG tick, in ms: what a phase length reported as zero stands for in
/// a ratio match. Such a phase is not instantaneous but over within one tick,
/// and the match needs a positive length.
inline constexpr double kInstantMs = 500.0 / eg_rate_hz(kNtscClockHz);

/// The candidate in `slowest`..`fastest` whose phase lasts closest to
/// `target_ms` in RATIO, `duration` being that phase's length in ms for one
/// candidate. A time axis reads logarithmically, so matching the linear
/// difference instead would put the whole fast end of the range out of a
/// drag's reach. Candidates whose phase never advances are skipped, and when
/// none advances the answer is the slowest; ties go to the slower value, so
/// dragging a handle outward does not stick.
template <typename Duration>
uint8_t nearest_rate(int slowest, int fastest, double target_ms,
                     Duration duration) {
  if (!(target_ms > 0.0) || !std::isfinite(target_ms)) {
    return static_cast<uint8_t>(fastest);
  }
  const auto log_length = [](double ms) {
    return std::log(std::max(ms, kInstantMs));
  };
  const double log_target = log_length(target_ms);
  int best = slowest;
  double best_distance = std::numeric_limits<double>::infinity();
  for (int candidate = slowest; candidate <= fastest; ++candidate) {
    const double ms = duration(candidate);
    if (!std::isfinite(ms)) {
      continue;
    }
    const double distance = std::fabs(log_length(ms) - log_target);
    if (distance < best_distance) {
      best_distance = distance;
      best = candidate;
    }
  }
  return static_cast<uint8_t>(best);
}

/// How long a release from full volume takes to reach silence, in ms. It is
/// the one phase PhaseDurations does not carry -- a held key never gets
/// there -- and like the sustain it is linear in attenuation, up to the level
/// at which the chip cuts the output dead. An inverted SSG-EG release is the
/// same length: key-off mirrors the level about the fold, so it climbs the
/// same distance.
inline double release_ms(const OperatorParams &op, NotePitch pitch) {
  const bool ssg = (op.ssg & 0x08) != 0;
  const int end_att =
      static_cast<int>(ssg ? kSsgFoldAttenuation : kCutAttenuation);
  const int rate = ym2612_eg::detail::effective_rate(
      2 * (op.rr & 0x0F) + 1, key_scale_value(op, pitch));
  return ym2612_eg::detail::linear_phase_ms(rate, 0, end_att, ssg,
                                            eg_rate_hz(kNtscClockHz));
}

/// Where a sustain at `op`'s SR stands `elapsed_ms` after it begins, in the
/// units the curve is drawn in: attenuation with TL added and the scale run
/// backwards for an SSG-EG mode that inverts, clamped exactly as the
/// simulator's output is. The phase is linear in attenuation, so its whole
/// length places every instant inside it -- including a rate that never
/// advances, whose length is infinite and whose level therefore never leaves
/// the sustain. Past the end the envelope is at rest.
inline double sustain_out_at_ms(const OperatorParams &op, NotePitch pitch,
                                double elapsed_ms) {
  const bool ssg = (op.ssg & 0x08) != 0;
  const int end_att =
      static_cast<int>(ssg ? kSsgFoldAttenuation : kCutAttenuation);
  const int sustain_att = std::min(sustain_attenuation(op.sl), end_att);
  const int rate = ym2612_eg::detail::effective_rate(
      op.sr & 0x1F, key_scale_value(op, pitch));
  const double whole_ms = ym2612_eg::detail::linear_phase_ms(
      rate, sustain_att, end_att, ssg, eg_rate_hz(kNtscClockHz));
  // Where the envelope stops: reaching the cut makes the chip force the bottom
  // of the scale, and an SSG-EG envelope freezes at the fold instead.
  const double rest_att =
      ssg ? static_cast<double>(end_att) : static_cast<double>(kMaxAttenuation);

  double att = static_cast<double>(sustain_att);
  if (elapsed_ms > 0.0) {
    att = elapsed_ms < whole_ms
              ? sustain_att + (end_att - sustain_att) * (elapsed_ms / whole_ms)
              : rest_att;
  }
  // Only an SSG-EG envelope inverts, and it never passes the fold, so the
  // simulator's `(0x200 - a) & 0x3FF` cannot wrap and is a subtraction.
  const double level = (ssg && (op.ssg & 0x04) != 0)
                           ? static_cast<double>(kSsgFoldAttenuation) - att
                           : att;
  return std::min(level + static_cast<double>(op.tl & 0x7F) * 8.0,
                  static_cast<double>(kMaxAttenuation));
}

} // namespace detail

/// The register value whose phase lasts closest to `target_ms` at `pitch`,
/// every other register left as `op` has it. `target_ms` is the length of
/// that phase alone, not a position on the time axis: the attack, the decay
/// down to the sustain level, and a release from full volume to silence.
///
/// AR and DR are searched over 1..31 and never answer 0, which is not a slow
/// rate but a phase that never advances -- whether a drag means that is the
/// caller's decision, not the library's. RR is 4 bits and its effective rate
/// is 2*RR+1, so every one of 0..15 finishes.
inline uint8_t solve_attack_rate(const OperatorParams &op, NotePitch pitch,
                                 double target_ms) {
  OperatorParams probe = op;
  return detail::nearest_rate(1, 31, target_ms, [&](int rate) {
    probe.ar = static_cast<uint8_t>(rate);
    return phase_durations(probe, pitch).attack_ms;
  });
}

inline uint8_t solve_decay_rate(const OperatorParams &op, NotePitch pitch,
                                double target_ms) {
  OperatorParams probe = op;
  return detail::nearest_rate(1, 31, target_ms, [&](int rate) {
    probe.dr = static_cast<uint8_t>(rate);
    const PhaseDurations phases = phase_durations(probe, pitch);
    // A decay that steps past the sustain window has no knee to answer with.
    const bool passes = ym2612_eg::detail::window_can_be_skipped(probe, pitch) &&
                        phases.sustain_ms == 0.0;
    return passes ? std::numeric_limits<double>::infinity() : phases.decay_ms;
  });
}

/// The sustain rate whose envelope sits closest to `out_attenuation` (output
/// units, TL included, 0..kMaxAttenuation) `elapsed_ms` after the sustain
/// begins. SR = 0 holds at the sustain level, so it is the answer for a level
/// that has not fallen at all.
///
/// The handle this inverts is dragged up and down a line rather than along
/// the axis, so nearest is measured in attenuation units, which is what the
/// graph's vertical axis is linear in. Ties go to the slower rate, so
/// dragging is not sticky. An elapsed time that is not one -- negative,
/// infinite, NaN -- is a sustain that has not advanced; a level off the scale
/// clamps onto it.
inline uint8_t solve_sustain_rate(const OperatorParams &op, NotePitch pitch,
                                  double elapsed_ms, double out_attenuation) {
  const double elapsed =
      elapsed_ms > 0.0 && std::isfinite(elapsed_ms) ? elapsed_ms : 0.0;
  const double target =
      out_attenuation > 0.0
          ? std::min(out_attenuation, static_cast<double>(kMaxAttenuation))
          : 0.0;
  OperatorParams probe = op;
  int best = 0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (int rate = 0; rate <= 31; ++rate) {
    probe.sr = static_cast<uint8_t>(rate);
    const double distance =
        std::fabs(detail::sustain_out_at_ms(probe, pitch, elapsed) - target);
    if (distance < best_distance) {
      best_distance = distance;
      best = rate;
    }
  }
  return static_cast<uint8_t>(best);
}

inline uint8_t solve_release_rate(const OperatorParams &op, NotePitch pitch,
                                  double target_ms) {
  OperatorParams probe = op;
  return detail::nearest_rate(0, 15, target_ms, [&](int rate) {
    probe.rr = static_cast<uint8_t>(rate);
    return detail::release_ms(probe, pitch);
  });
}

/// The total level nearest `out_attenuation`: TL is the top 7 bits of the
/// 10-bit attenuation, so one step is 8 units. Anything off the scale, NaN
/// included, clamps.
inline uint8_t solve_total_level(double out_attenuation) {
  const double steps = out_attenuation / 8.0;
  if (!(steps > 0.0)) {
    return 0;
  }
  if (!(steps < 127.0)) {
    return 127;
  }
  return static_cast<uint8_t>(std::lround(steps));
}

/// The sustain level whose attenuation, TL included, is nearest
/// `out_attenuation`. The levels are 32 units apart except SL = 15, which is
/// 0x3E0 rather than 480; ties go to the louder level.
inline uint8_t solve_sustain_level(const OperatorParams &op,
                                   double out_attenuation) {
  const double tl_att = static_cast<double>(op.tl & 0x7F) * 8.0;
  int best = 0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (int sl = 0; sl < 16; ++sl) {
    const double distance =
        std::fabs(static_cast<double>(sustain_attenuation(sl)) + tl_att -
                  out_attenuation);
    if (distance < best_distance) {
      best_distance = distance;
      best = sl;
    }
  }
  return static_cast<uint8_t>(best);
}

} // namespace ym2612_eg::graph
