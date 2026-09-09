#pragma once

// The envelope graph's drag handles: where each one sits, which of them can be
// dragged honestly, and what one pointer movement off one asks the registers
// for. Milliseconds, attenuation and pixels only -- what draws the dots, what
// puts a button on them and what writes the registers are all elsewhere.

#include "graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ym2612_eg::graph {

/// A place on the plot, in the pixels the graph is drawn in.
struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

/// Attenuation at the bottom of the graph; 0 (full volume) is at the top.
inline constexpr double kFullScale = static_cast<double>(kMaxAttenuation);

/// A handle is named for the part of the envelope it is grabbed by, and
/// stands for the one or two parameters that part is made of.
enum HandleIndex {
  kAttackHandle = 0, ///< AttackRate across, TotalLevel down
  kDecayHandle,      ///< DecayRate across, SustainLevel down
  kSustainHandle,    ///< SustainRate down
  kReleaseHandle,    ///< ReleaseRate across
  kHandleCount,
};

/// The plot in pixels against the units the curve is drawn in: milliseconds
/// across, attenuation down.
struct PlotArea {
  Point min;
  Point max;
  /// The width being drawn, which stands still while a handle is held.
  double span_ms = 1.0;

  float width() const { return std::max(max.x - min.x, 1.0f); }
  float height() const { return std::max(max.y - min.y, 1.0f); }

  float x_of(double ms) const {
    const double t = std::clamp(ms / span_ms, 0.0, 1.0);
    return min.x + static_cast<float>(t) * width();
  }
  float y_of(double out) const {
    const double t = std::clamp(out / kFullScale, 0.0, 1.0);
    return min.y + static_cast<float>(t) * height();
  }
  Point at(double ms, double out) const { return Point{x_of(ms), y_of(out)}; }

  /// What one pixel of pointer movement is worth.
  double ms_per_px() const { return span_ms / static_cast<double>(width()); }
  double out_per_px() const {
    return kFullScale / static_cast<double>(height());
  }
};

/// The sizes a handle is drawn and grabbed at, in the pixels the graph is
/// drawn in, and the least room a sustain has to have to carry one.
struct HandleMetrics {
  float radius = 3.0f;
  float grab = 6.0f;
  float min_sustain_height = 6.0f;
  float min_sustain_width = 8.0f;
};

struct EnvelopeHandle {
  /// False for a handle that cannot be dragged honestly: it is neither drawn
  /// nor hit-tested.
  bool shown = false;
  /// Where it is drawn, clamped so the whole dot stays inside the plot.
  Point pos;
  /// What the across parameter's solver is asked about: the length of that
  /// phase alone, or -- for the sustain, which has no end to grab -- how far
  /// into the sustain `pos` reads the line.
  double ms = 0.0;
  /// What the down parameter's solver is asked about.
  double out = 0.0;
  /// Where the dot stands on the axis, in the milliseconds the graph is drawn
  /// in. `ms` is what the solver is told; these are what the pointer moves.
  double at_ms = 0.0;
  /// Where the phase this handle belongs to leaves its start, in the same
  /// drawn units and in attenuation. A pointer anywhere along the line from
  /// there names an angle, and the angle names the rate.
  double anchor_ms = 0.0;
  double anchor_out = 0.0;
  /// False for a handle waiting somewhere the curve does not put it: a decay
  /// that never ends waits at the right-hand edge, and one with no length at
  /// all waits beside the peak. A drag from there still sets the register,
  /// but reading it back does not answer the value it came from.
  bool parked = false;
  /// How much `ms` moves per millisecond the dot does. One wherever the dot
  /// stands on what it sets; a release ends on the graph before its own trace
  /// does whenever TL lifts the floor, so a millisecond there is worth more
  /// than a millisecond of release.
  double ms_per_drawn = 1.0;
};

struct EnvelopeHandles {
  PlotArea plot;
  HandleMetrics metrics;
  EnvelopeHandle items[kHandleCount];
};

namespace detail {

/// The whole dot inside the plot: a handle in a corner is still a dot rather
/// than a quarter of one, and its grab box is not half off the graph.
inline Point inside(const PlotArea &plot, Point pos, float radius) {
  return Point{std::clamp(pos.x, plot.min.x + radius, plot.max.x - radius),
               std::clamp(pos.y, plot.min.y + radius, plot.max.y - radius)};
}

} // namespace detail

/// Where the sustain handle reads the line: half way along the part of it that
/// can still be seen falling.
inline double sustain_probe_ms(const EnvelopeCurve &curve, double span_ms) {
  const double start = std::max(curve.decay_end_ms, 0.0);
  // Half way along the part of the sustain the eye can still see falling. Past
  // the floor the line is flat and every rate lies on top of every other, so a
  // dot there would stand nowhere in particular.
  const double end =
      std::min(first_time_at_level(curve.held, kFullScale, start, span_ms),
               span_ms);
  return start + (end - start) * 0.5;
}

/// The handle nearest `pos` and within its grab box, or `kHandleCount` for
/// none. Two handles can stand close enough to share a grab box -- a shallow
/// sustain level puts the knee just under the peak -- so which one answers is
/// decided by distance rather than by whichever is offered first.
inline HandleIndex nearest_handle(const EnvelopeHandles &handles, Point pos) {
  HandleIndex nearest = kHandleCount;
  float best = handles.metrics.grab * handles.metrics.grab;
  for (int i = 0; i < kHandleCount; ++i) {
    const EnvelopeHandle &item = handles.items[i];
    if (!item.shown) {
      continue;
    }
    const float dx = item.pos.x - pos.x;
    const float dy = item.pos.y - pos.y;
    const float distance = dx * dx + dy * dy;
    if (distance <= best) {
      best = distance;
      nearest = static_cast<HandleIndex>(i);
    }
  }
  return nearest;
}

inline EnvelopeHandles handle_layout(const EnvelopeCurve &curve,
                                     const PlotArea &plot, bool ssg_enabled,
                                     const HandleMetrics &metrics) {
  EnvelopeHandles out;
  out.plot = plot;
  out.metrics = metrics;
  // An SSG-EG envelope is not four parameters laid end to end: the trace folds,
  // and no point on it stands for one register.
  if (ssg_enabled) {
    return out;
  }

  const double span = plot.span_ms;
  // `ms` is what the solver is asked about; `at_ms` is where on the axis that
  // puts the dot. They differ wherever a phase does not start at zero.
  const auto place = [&](HandleIndex index, double ms, double at_ms,
                         double out_att, double ms_per_drawn = 1.0,
                         bool parked = false, double anchor_ms = 0.0,
                         double anchor_out = 0.0) {
    EnvelopeHandle &handle = out.items[index];
    handle.shown = true;
    handle.ms = ms;
    handle.out = out_att;
    handle.at_ms = at_ms;
    handle.anchor_ms = anchor_ms;
    handle.anchor_out = anchor_out;
    handle.parked = parked;
    handle.ms_per_drawn = ms_per_drawn;
    handle.pos = detail::inside(plot, plot.at(at_ms, out_att), metrics.radius);
  };

  // The peak: TotalLevel is the level it stands at, AttackRate the time it
  // took to get there.
  if (curve.attack_end_ms >= 0.0 && curve.attack_end_ms <= span) {
    place(kAttackHandle, curve.attack_end_ms, curve.attack_end_ms,
          curve.peak_out);
  }

  // The knee, whose time is the decay's own length rather than where it falls
  // on the axis. A slow decay can end past the axis, and then the knee waits
  // at the right-hand edge on the line it would leave.
  if (curve.attack_end_ms >= 0.0 && curve.attack_end_ms <= span) {
    // Just clear of the peak, where the decay would begin. A decay rate of 0
    // never advances and a sustain level of 0 has nowhere to fall to: either
    // way there is no knee on the line, and this is where pulling one out
    // starts.
    const double clear_ms =
        curve.attack_end_ms +
        (metrics.grab + metrics.radius) * plot.ms_per_px();
    const bool has_knee = curve.decay_end_ms >= 0.0;
    const bool knee_on_axis = has_knee && curve.decay_end_ms <= span;
    const double out_att = knee_on_axis ? curve.sustain_out
                                        : curve_out_at_ms(curve.held, span);
    // Where the eye finds the knee, which comes before the decay's own end
    // whenever the output saturates on the way down: TL lifts the whole
    // envelope, so a high sustain level is already at the floor of the graph
    // while the attenuation still has ground to cover.
    const double at_ms =
        has_knee ? first_time_at_level(curve.held, out_att, curve.attack_end_ms,
                                       knee_on_axis ? curve.decay_end_ms : span)
                 : clear_ms;
    const Point peak = plot.at(curve.attack_end_ms, curve.peak_out);
    const Point knee = plot.at(at_ms, out_att);
    const bool clear_of_peak = std::abs(knee.x - peak.x) >= metrics.radius ||
                               std::abs(knee.y - peak.y) >= metrics.radius;
    if (has_knee && clear_of_peak) {
      const double decay_ms =
          (knee_on_axis ? curve.decay_end_ms : span) - curve.attack_end_ms;
      const double drawn_ms = at_ms - curve.attack_end_ms;
      place(kDecayHandle, decay_ms, at_ms, out_att,
            drawn_ms > 0.0 ? decay_ms / drawn_ms : 1.0, !knee_on_axis,
            curve.attack_end_ms, curve.peak_out);
    } else {
      place(kDecayHandle, clear_ms - curve.attack_end_ms, clear_ms,
            curve_out_at_ms(curve.held, clear_ms), 1.0, true,
            curve.attack_end_ms, curve.peak_out);
    }
  }

  // The sustain has no corner to grab, so the handle sits at a fixed instant
  // along it and carries the level the trace is actually drawn at there. Too
  // thin or too narrow and there is nothing to drag it through.
  if (curve.decay_end_ms >= 0.0 && curve.decay_end_ms < span) {
    const double at_ms =
        std::max(sustain_probe_ms(curve, span),
                 curve.decay_end_ms + metrics.grab * plot.ms_per_px());
    const float room_x = plot.x_of(span) - plot.x_of(curve.decay_end_ms);
    const float room_y = plot.y_of(kFullScale) - plot.y_of(curve.sustain_out);
    // Room enough to drag the line through means the dot stands on what it
    // sets; without it -- a sustain level of 15 leaves a sliver -- it waits
    // there instead, which is still where tilting one starts.
    const bool has_room = room_x >= metrics.min_sustain_width &&
                          room_y >= metrics.min_sustain_height;
    place(kSustainHandle, at_ms - curve.decay_end_ms, at_ms,
          curve_out_at_ms(curve.held, at_ms), 1.0, !has_room,
          curve.decay_end_ms, curve.sustain_out);
  }

  // Where the release reaches the floor of the graph. It falls at one rate
  // from full volume, and TL lifts the whole envelope: the output saturates
  // while the attenuation still has ground to cover, so the fall the eye sees
  // is the shorter of the two by exactly that ratio. The dot stands on what
  // the eye sees and the solver is told about the release behind it.
  if (curve.release_content_ms > 0.0) {
    const double drawn_fall = kFullScale - static_cast<double>(curve.peak_out);
    const double scale = drawn_fall > 0.0
                             ? static_cast<double>(kCutAttenuation) / drawn_fall
                             : 1.0;
    // A release that outran the simulation ends where the budget did, not
    // where the release does, so its floor is not on the graph at all. Either
    // way the dot waits on the line -- at the end of what is drawn, or at the
    // right-hand edge -- because tilting it there is what brings the end back
    // into view.
    const double floor_ms = curve.release_content_ms / scale;
    const bool ends_on_axis = !curve.release_truncated && floor_ms <= span;
    const double at_ms =
        ends_on_axis
            ? floor_ms
            : std::min(curve.release_truncated ? curve.release_content_ms
                                               : span,
                       span);
    place(kReleaseHandle, curve.release_content_ms, at_ms,
          ends_on_axis ? kFullScale : curve_out_at_ms(curve.release, at_ms),
          scale, !ends_on_axis, 0.0, curve.peak_out);
  }

  return out;
}

/**
 * A drag is measured from where it was grabbed rather than from where the
 * pointer is: `moved_px` is the distance travelled since, and the value it
 * arrives at is the one the handle was grabbed at plus that much. A pointer
 * that has not moved therefore asks for the value the handle already holds.
 */
inline double dragged_ms(const PlotArea &plot, double grabbed_ms, float moved_px,
                         double ms_per_drawn = 1.0) {
  return grabbed_ms +
         static_cast<double>(moved_px) * plot.ms_per_px() * ms_per_drawn;
}

inline double dragged_out(const PlotArea &plot, double grabbed_out,
                          float moved_px) {
  return grabbed_out + static_cast<double>(moved_px) * plot.out_per_px();
}

// ------------------------------------------------ from a drag to a register

enum class HandleField : uint8_t {
  AttackRate,
  DecayRate,
  SustainLevel,
  SustainRate,
  ReleaseRate,
  TotalLevel,
};

struct HandleWrite {
  HandleField field;
  uint8_t value;
};

/// The registers one pointer movement asks for, in the order they are to be
/// applied.
struct HandleEdit {
  std::array<HandleWrite, 2> writes{};
  int count = 0;
};

/// Everything a drag needs, frozen on the frame the handle is grabbed: the
/// handle as it stood and the plot it stood on.
struct HandleGrab {
  HandleIndex handle = kHandleCount;
  EnvelopeHandle grabbed;
  PlotArea plot;
};

inline HandleGrab grab_handle(const EnvelopeHandles &handles,
                              HandleIndex handle) {
  HandleGrab grab;
  if (handle >= kHandleCount) {
    return grab;
  }
  grab.handle = handle;
  grab.grabbed = handles.items[handle];
  grab.plot = handles.plot;
  return grab;
}

/// What `moved_px` of pointer movement off `grab` asks `op` to become. A
/// pointer that has not moved asks for nothing at all, so a click writes no
/// register; neither does a grab of a handle that was never offered.
inline HandleEdit drag_handle(const HandleGrab &grab, const OperatorParams &op,
                              NotePitch pitch, Point moved_px) {
  HandleEdit edit;
  if (grab.handle >= kHandleCount || !grab.grabbed.shown ||
      (moved_px.x == 0.0f && moved_px.y == 0.0f)) {
    return edit;
  }

  const EnvelopeHandle &drag = grab.grabbed;
  const PlotArea &plot = grab.plot;
  // Where the pointer has got to, in the units the graph is drawn in.
  const double at_ms = dragged_ms(plot, drag.at_ms, moved_px.x);
  const double level = dragged_out(plot, drag.out, moved_px.y);
  const auto write = [&edit](HandleField field, int value) {
    edit.writes[static_cast<std::size_t>(edit.count++)] =
        HandleWrite{field, static_cast<uint8_t>(value)};
  };

  switch (grab.handle) {
  case kAttackHandle:
    // A corner: the two axes are the two parameters, how long across and how
    // loud down.
    if (moved_px.x != 0.0f) {
      write(HandleField::AttackRate,
            solve_attack_rate(op, pitch,
                              dragged_ms(plot, drag.ms, moved_px.x,
                                         drag.ms_per_drawn)));
    }
    if (moved_px.y != 0.0f) {
      write(HandleField::TotalLevel, solve_total_level(level));
    }
    break;
  case kDecayHandle: {
    // The level first: how long a decay lasts is measured to where it is
    // going, so the rate has to be solved against the level just set.
    OperatorParams aimed = op;
    if (moved_px.y != 0.0f) {
      const int sustain_level = solve_sustain_level(op, level);
      // A knee dragged up onto the peak leaves no decay to describe. The
      // register for that is a decay rate of 0, which never advances: the
      // envelope holds where the attack left it. The sustain level stays one
      // step below the peak so the knee has somewhere to come back to.
      if (sustain_level <= 0) {
        write(HandleField::SustainLevel, 1);
        write(HandleField::DecayRate, 0);
        return edit;
      }
      write(HandleField::SustainLevel, sustain_level);
      aimed.sl = static_cast<uint8_t>(sustain_level);
    }
    if (moved_px.x != 0.0f) {
      write(HandleField::DecayRate,
            solve_decay_rate(aimed, pitch,
                             (at_ms - drag.anchor_ms) * drag.ms_per_drawn));
    }
    break;
  }
  case kSustainHandle:
  case kReleaseHandle: {
    // The line is pinned where its phase begins, so the pointer names an
    // angle. A sustain answers the level it has reached by then; a release
    // answers where that angle would put the floor. Never at the instant it is
    // pinned, where every rate passes through the one point.
    const double elapsed = std::max(at_ms - drag.anchor_ms, plot.ms_per_px());
    if (grab.handle == kSustainHandle) {
      write(HandleField::SustainRate,
            solve_sustain_rate(op, pitch, elapsed, level));
      break;
    }
    const double fall = level - drag.anchor_out;
    // A pointer level with the pinned end, or above it, tilts a line that
    // never reaches the floor: the slowest release is what it is asking for.
    if (!(fall > 1.0)) {
      write(HandleField::ReleaseRate, 0);
      break;
    }
    const double reach = elapsed * (kFullScale - drag.anchor_out) / fall;
    write(HandleField::ReleaseRate,
          solve_release_rate(op, pitch, reach * drag.ms_per_drawn));
    break;
  }
  default:
    break;
  }
  return edit;
}

} // namespace ym2612_eg::graph
