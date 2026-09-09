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
#include <vector>

namespace ym2612_eg::graph {

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

/// How much of the held envelope is worth seeing at `pitch`, in ms: about
/// kSsgLoopPeriods periods of an SSG loop, or otherwise the envelope's own
/// phase durations via window_for_timeline_ms(). A scale, not a length: the
/// envelope is drawn across the whole axis chosen from it. Nothing here is a
/// key-off.
inline double choose_held_ms(const OperatorParams &op, NotePitch pitch) {
  const double period = ssg_loop_period_ms(op, pitch);
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

/// The one warning worth showing, or nullptr: an SSG-EG mode driven by an
/// attack rate the hardware convention says should be 31. Everything else the
/// simulator flags is already visible in the shape of the curve.
inline const char *warning_line(const CurveResult &curve) {
  for (const CurveWarning w : curve.warnings) {
    if (w == CurveWarning::SsgArBelow31) {
      return "AR<31: non-standard SSG-EG";
    }
  }
  return nullptr;
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

  const char *warning = nullptr;
};

/// Two passes over the simulator: a release from full volume, which shares
/// only the time axis with the other; and -- once the window policy and the
/// release have decided how wide the axis is -- the held trace, simulated
/// across the whole of it so a loop keeps looping to the right edge.
/// `min_span_ms` is the axis the curve will actually be DRAWN on, which for a
/// voice overlay is another curve's rather than its own; the held trace is
/// simulated across at least that much, because a sawtooth extrapolated along
/// the slope of its last ramp is not a sawtooth. A pure function of the
/// operator and the note.
inline EnvelopeCurve build_envelope_curve(const OperatorParams &op,
                                          NotePitch pitch,
                                          double min_span_ms = 0.0) {
  EnvelopeCurve out;

  // 1. What the axis has to hold, in closed form over the registers alone.
  const double period_ms = ssg_loop_period_ms(op, pitch);
  const bool loops = period_ms > 0.0 && std::isfinite(period_ms);
  out.held_ms = choose_held_ms(op, pitch);

  // 2. The release, on its own: keyed on at full volume and released at once,
  //    which routes it through the chip's real key-off rules -- the SSG
  //    inversion latch, the 4x increments, the hard cut at 0x200. The gate is
  //    a sample rather than zero because a key write takes a sample to reach
  //    the envelope; released on sample zero the note never starts.
  //
  //    "Full volume" is one step short of loudest_attenuation(), not 0: with
  //    an inverted SSG-EG mode 0 is the quiet end of the ramp, and the loudest
  //    attenuation is the fold level itself, which an operator only ever
  //    passes through. AR is zeroed for this run alone -- key_on() snaps an
  //    instant attack straight to att = 0 and would throw the start level
  //    away, and the release rate does not depend on AR.
  CurveRequest release;
  release.op = op;
  release.op.ar = 0;
  release.pitch = pitch;
  release.gate_ms = 2000.0 / sample_rate_hz(kNtscClockHz);
  release.max_ms = release_max_ms();
  const uint16_t loudest = loudest_attenuation(op);
  release.start_att = loudest > 0 ? static_cast<uint16_t>(loudest - 1) : 0;
  out.release = sample_curve(release);
  // The key reaches the envelope one sample after the write, so the samples
  // before that carry a level the note never sounds at: drop them and put the
  // first sounding one at the origin.
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

  out.held_tail_slope = detail::tail_slope(out.held, out.held_parked);
  out.release_tail_slope =
      detail::tail_slope(out.release, !out.release_truncated);

  const int tl_att = static_cast<int>(op.tl) * 8;
  out.peak_out =
      static_cast<uint16_t>(std::min(tl_att, static_cast<int>(kMaxAttenuation)));
  // The level the decay aims at, in the same output units as the curve.
  out.sustain_out = static_cast<uint16_t>(
      std::min(sustain_attenuation(op.sl) + tl_att,
               static_cast<int>(kMaxAttenuation)));

  out.warning = warning_line(out.held);
  return out;
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

/// Where on a release trace a release from attenuation `out` begins: the
/// release is linear in attenuation, so a note let go at level L follows
/// precisely the trace already on screen, entered later -- the first instant
/// the trace reaches `out` IS where the voice joins it. Takes the trace rather
/// than the whole curve, so the same question can be put to a release the
/// simulator really ran.
inline double release_entry_ms(const CurveResult &release, double out) {
  const auto &points = release.points;
  if (points.empty()) {
    return 0.0;
  }
  if (out <= points.front().out) {
    return points.front().ms;
  }
  // A release trace is a straight ramp in attenuation and RDP decimates it to
  // a handful of vertices, so interpolating inside the crossing edge is exact.
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double a0 = points[i - 1].out;
    const double a1 = points[i].out;
    if (a1 < out) {
      continue;
    }
    if (a1 <= a0) {
      return points[i].ms; // a step, not a ramp: it arrives at this instant
    }
    const double u = std::clamp((out - a0) / (a1 - a0), 0.0, 1.0);
    return points[i - 1].ms +
           (static_cast<double>(points[i].ms) - points[i - 1].ms) * u;
  }
  return points.back().ms;
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

/// Half an EG tick, in ms: the shortest length the closed forms tell apart.
/// A phase they report as zero is not instantaneous but over within one tick,
/// and a ratio match needs a positive length to stand in for it.
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
    return phase_durations(probe, pitch).decay_ms;
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
