// ym2612_eg::graph's drag handles: where each one stands on the plot, which of
// them are offered, and what one pointer movement off one asks the registers
// for.

#include "check.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

using namespace ym2612_eg;
using namespace ym2612_eg::graph;

namespace {

/// Every curve here is drawn at middle C.
const NotePitch kMiddleC = NotePitch::from_midi(60);

OperatorParams adsr(int ar, int dr, int sl, int sr, int rr, int tl) {
  OperatorParams op;
  op.ar = static_cast<uint8_t>(ar);
  op.dr = static_cast<uint8_t>(dr);
  op.sl = static_cast<uint8_t>(sl);
  op.sr = static_cast<uint8_t>(sr);
  op.rr = static_cast<uint8_t>(rr);
  op.tl = static_cast<uint8_t>(tl);
  return op;
}

/// EG_SPEC's worked example: AR=31 TL=0 DR=10 SL=2 SR=5 RR=7, KS=0.
OperatorParams worked_example() { return adsr(31, 10, 2, 5, 7, 0); }

EnvelopeCurve curve_of(const OperatorParams &op) {
  return build_envelope_curve(op, kMiddleC);
}

PlotArea plot_over(const EnvelopeCurve &curve) {
  PlotArea plot;
  plot.min = Point{20.0f, 10.0f};
  plot.max = Point{220.0f, 110.0f}; // 200 x 100
  plot.span_ms = curve.span_ms;
  return plot;
}

HandleMetrics metrics() { return HandleMetrics{}; }

EnvelopeHandles layout_of(const EnvelopeCurve &curve) {
  return handle_layout(curve, plot_over(curve), metrics());
}

bool near_abs(double a, double b, double tolerance) {
  return std::fabs(a - b) <= tolerance;
}

/// `op` with one handle's register written.
OperatorParams applied(const OperatorParams &op, HandleField field,
                       uint8_t value) {
  OperatorParams out = op;
  switch (field) {
  case HandleField::AttackRate:
    out.ar = value;
    break;
  case HandleField::DecayRate:
    out.dr = value;
    break;
  case HandleField::SustainLevel:
    out.sl = value;
    break;
  case HandleField::SustainRate:
    out.sr = value;
    break;
  case HandleField::ReleaseRate:
    out.rr = value;
    break;
  case HandleField::TotalLevel:
    out.tl = value;
    break;
  }
  return out;
}

/// The register `field` takes to land nearest `target` -- milliseconds for a
/// rate, attenuation for a level. `elapsed_ms` is how far into the sustain the
/// level was read and means nothing to the other fields.
int solve_field(const OperatorParams &op, HandleField field, double target,
                double elapsed_ms) {
  switch (field) {
  case HandleField::AttackRate:
    return solve_attack_rate(op, kMiddleC, target);
  case HandleField::DecayRate:
    return solve_decay_rate(op, kMiddleC, target);
  case HandleField::SustainLevel:
    return solve_sustain_level(op, target);
  case HandleField::SustainRate:
    return solve_sustain_rate(op, kMiddleC, elapsed_ms, target);
  case HandleField::ReleaseRate:
    return solve_release_rate(op, kMiddleC, target);
  case HandleField::TotalLevel:
    return solve_total_level(target);
  }
  return 0;
}

EnvelopeCurve solved_curve(const OperatorParams &op, HandleField field,
                           double target, double elapsed_ms) {
  return curve_of(applied(
      op, field,
      static_cast<uint8_t>(solve_field(op, field, target, elapsed_ms))));
}

// --------------------------------------------------- pixels against values

void test_the_plot_maps_the_curve_onto_its_own_rectangle() {
  PlotArea plot;
  plot.min = Point{20.0f, 10.0f};
  plot.max = Point{220.0f, 110.0f};
  plot.span_ms = 1000.0;

  CHECK(plot.x_of(0.0) == 20.0f);
  CHECK(plot.x_of(500.0) == 120.0f);
  CHECK(plot.x_of(1000.0) == 220.0f);
  // Off either end of the axis stops at the edge rather than smearing past it.
  CHECK(plot.x_of(-10.0) == 20.0f);
  CHECK(plot.x_of(4000.0) == 220.0f);

  CHECK(plot.y_of(0.0) == 10.0f); // full volume is the top
  CHECK(plot.y_of(kFullScale) == 110.0f);

  CHECK(near_abs(plot.ms_per_px(), 5.0, 1e-9));
  CHECK(near_abs(plot.out_per_px(), kFullScale / 100.0, 1e-9));
}

void test_a_drag_that_has_not_moved_asks_for_what_it_grabbed() {
  PlotArea plot;
  plot.min = Point{20.0f, 10.0f};
  plot.max = Point{220.0f, 110.0f};
  plot.span_ms = 1000.0;

  CHECK(dragged_ms(plot, 123.5, 0.0f) == 123.5);
  CHECK(dragged_out(plot, 456.5, 0.0f) == 456.5);

  // And a pointer that has moved lands one axis-worth per pixel away from it,
  // in both directions.
  CHECK(near_abs(dragged_ms(plot, 100.0, 10.0f), 150.0, 1e-9));
  CHECK(near_abs(dragged_ms(plot, 100.0, -10.0f), 50.0, 1e-9));
  CHECK(near_abs(dragged_out(plot, 100.0, 10.0f), 100.0 + kFullScale * 0.1,
                 1e-9));
}

// ------------------------------------------------------- where they stand

void test_every_handle_stands_where_the_curve_puts_it() {
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = curve_of(op);
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandles handles = layout_of(curve);

  const EnvelopeHandle &attack = handles.items[kAttackHandle];
  CHECK(attack.shown);
  // The attack starts at zero, so its length and its place on the axis are
  // the same number.
  CHECK(attack.ms == curve.attack_end_ms);
  CHECK(attack.out == curve.peak_out);

  const EnvelopeHandle &decay = handles.items[kDecayHandle];
  CHECK(decay.shown);
  // The decay's own length, not where it lands on the axis.
  CHECK(near_abs(decay.ms, curve.decay_end_ms - curve.attack_end_ms, 1e-9));
  CHECK(decay.out == curve.sustain_out);
  CHECK(near_abs(decay.pos.x, plot.x_of(curve.decay_end_ms), 0.001));

  const EnvelopeHandle &sustain = handles.items[kSustainHandle];
  CHECK(sustain.shown);
  const double probe = sustain_probe_ms(curve, plot.span_ms);
  CHECK(near_abs(probe, (curve.decay_end_ms + curve.span_ms) * 0.5, 1e-9));
  CHECK(near_abs(sustain.ms, probe - curve.decay_end_ms, 1e-9));
  // Read off the trace that is drawn rather than off a closed form.
  CHECK(near_abs(sustain.out, curve_out_at_ms(curve.held, probe), 1e-9));

  const EnvelopeHandle &release = handles.items[kReleaseHandle];
  CHECK(release.shown);
  CHECK(release.ms == curve.release_content_ms);
  CHECK(release.out == kFullScale);
}

void test_the_whole_dot_stays_inside_the_plot() {
  // TL 0 puts the peak on the top edge and AR 31 puts it on the left one.
  const EnvelopeCurve curve = curve_of(worked_example());
  const PlotArea plot = plot_over(curve);
  const HandleMetrics sizes = metrics();
  const EnvelopeHandles handles = handle_layout(curve, plot, sizes);

  for (int i = 0; i < kHandleCount; ++i) {
    const EnvelopeHandle &handle = handles.items[i];
    if (!handle.shown) {
      continue;
    }
    CHECK(handle.pos.x >= plot.min.x + sizes.radius);
    CHECK(handle.pos.x <= plot.max.x - sizes.radius);
    CHECK(handle.pos.y >= plot.min.y + sizes.radius);
    CHECK(handle.pos.y <= plot.max.y - sizes.radius);
  }
}

// ------------------------------------------------------------------ SSG-EG

/// Fine enough for a drag to name any register value.
PlotArea fine_plot(double span_ms) {
  PlotArea plot;
  plot.min = Point{0.0f, 0.0f};
  plot.max = Point{4000.0f, 2000.0f};
  plot.span_ms = span_ms;
  return plot;
}

OperatorParams ssg_patch(int mode, int ar, int tl) {
  OperatorParams op = adsr(ar, 10, 4, 5, 7, tl);
  op.ssg = static_cast<uint8_t>(0x08 | mode);
  return op;
}

void test_every_ssg_dot_stands_on_the_line_it_edits() {
  for (int mode = 0; mode < 8; ++mode) {
    for (int ar : {31, 28, 20, 10}) {
      for (int tl : {0, 8, 33}) {
        const OperatorParams op = ssg_patch(mode, ar, tl);
        const EnvelopeCurve curve = curve_of(op);
        const PlotArea plot = plot_over(curve);
        const EnvelopeHandles handles = handle_layout(curve, plot, metrics());
        CHECK(handles.items[kAttackHandle].shown);
        CHECK(handles.items[kDecayHandle].shown);
        CHECK(handles.items[kSustainHandle].shown);
        CHECK(handles.items[kReleaseHandle].shown);
        for (const HandleIndex index :
             {kAttackHandle, kDecayHandle, kSustainHandle}) {
          const EnvelopeHandle &handle = handles.items[index];
          const float line =
              plot.y_of(curve_out_at_ms(curve.held, handle.at_ms));
          CHECK(near_abs(handle.pos.y, line, metrics().radius + 2.0));
          // On the first ramp.
          CHECK(handle.at_ms <= curve.ramp_end_ms + 1.0 || handle.parked);
        }
        const EnvelopeHandle &release = handles.items[kReleaseHandle];
        // Just before the cut.
        CHECK(near_abs(
            release.pos.y,
            plot.y_of(curve_out_at_ms(curve.release, release.at_ms - 0.1)),
            metrics().radius + 2.0));
      }
    }
  }
}

void test_a_ramp_with_no_sustain_ends_in_its_decay() {
  OperatorParams op = ssg_patch(2, 31, 8);
  op.sl = 15;
  const EnvelopeCurve curve = curve_of(op);
  CHECK(curve.decay_end_ms < 0.0);
  const EnvelopeHandles handles = layout_of(curve);
  CHECK(!handles.items[kSustainHandle].shown);
  const EnvelopeHandle &decay = handles.items[kDecayHandle];
  CHECK(decay.shown);
  CHECK(!decay.parked);
  CHECK(near_abs(decay.at_ms, curve.ramp_end_ms, 1e-9));
  CHECK(near_abs(decay.out, curve.ramp_end_out, 1e-9));
}

void test_an_attack_past_the_axis_waits_at_its_edge() {
  const OperatorParams op = ssg_patch(0, 6, 8);
  const EnvelopeCurve curve = curve_of(op);
  PlotArea plot = plot_over(curve);
  plot.span_ms = curve.attack_ms * 0.25;
  const EnvelopeHandles handles = handle_layout(curve, plot, metrics());
  const EnvelopeHandle &attack = handles.items[kAttackHandle];
  CHECK(attack.shown);
  CHECK(attack.parked);
  CHECK(near_abs(attack.at_ms, plot.span_ms, 1e-9));
  CHECK(near_abs(attack.ms, curve.attack_ms, 1e-9));
}

void test_a_knee_pulled_onto_an_ssg_peak_keeps_the_loop_running() {
  const OperatorParams op = ssg_patch(0, 31, 8);
  const EnvelopeCurve curve = curve_of(op);
  const PlotArea plot = fine_plot(curve.span_ms);
  const EnvelopeHandles handles = handle_layout(curve, plot, metrics());
  const HandleGrab grab = grab_handle(handles, kDecayHandle);
  const HandleEdit edit =
      drag_handle(grab, op, kMiddleC, Point{0.0f, -2000.0f});
  CHECK(edit.count == 1);
  CHECK(edit.writes[0].field == HandleField::SustainLevel);
  CHECK(edit.writes[0].value == 0);
}

/// The drag from `from`'s dot to `to`'s.
Point moved_to(const EnvelopeHandles &from, const EnvelopeHandles &to,
               HandleIndex index) {
  const Point a = from.plot.at(from.items[index].at_ms, from.items[index].out);
  const Point b = to.plot.at(to.items[index].at_ms, to.items[index].out);
  return Point{b.x - a.x, b.y - a.y};
}

/// A drag to another value's dot asks for that value, in every mode.
void test_a_drag_to_another_values_dot_asks_for_that_value() {
  for (int mode = 0; mode < 8; ++mode) {
    // AR stays below 31: from 31 an alternate mode flips.
    const OperatorParams op = ssg_patch(mode, 22, 8);
    const auto layout_for = [&](const OperatorParams &patch, double span) {
      return handle_layout(curve_of(patch), fine_plot(span), metrics());
    };
    const double span = curve_of(op).span_ms * 2.0;
    const EnvelopeHandles from = layout_for(op, span);

    const auto check_value = [&](HandleIndex index, HandleField field,
                                 const OperatorParams &target, int value) {
      const EnvelopeHandles to = layout_for(target, span);
      const HandleEdit edit = drag_handle(grab_handle(from, index), op, kMiddleC,
                                          moved_to(from, to, index));
      bool found = false;
      for (int i = 0; i < edit.count; ++i) {
        const HandleWrite write = edit.writes[static_cast<std::size_t>(i)];
        if (write.field == field) {
          found = true;
          CHECK(write.value == value);
        }
      }
      CHECK(found);
    };

    OperatorParams target = op;
    target.ar = 20;
    check_value(kAttackHandle, HandleField::AttackRate, target, 20);
    target = op;
    target.tl = 20;
    check_value(kAttackHandle, HandleField::TotalLevel, target, 20);
    target = op;
    target.dr = 12;
    check_value(kDecayHandle, HandleField::DecayRate, target, 12);
    target = op;
    target.sl = 7;
    check_value(kDecayHandle, HandleField::SustainLevel, target, 7);
    target = op;
    target.rr = 9;
    check_value(kReleaseHandle, HandleField::ReleaseRate, target, 9);

    // The level the line reaches under the dot, with SR 6 still on its first
    // ramp there.
    target = op;
    target.sr = 6;
    const EnvelopeCurve tilted = curve_of(target);
    const EnvelopeHandle &dot = from.items[kSustainHandle];
    const Point moved{
        0.0f, from.plot.y_of(curve_out_at_ms(tilted.held, dot.at_ms)) -
                  from.plot.y_of(dot.out)};
    const HandleEdit edit =
        drag_handle(grab_handle(from, kSustainHandle), op, kMiddleC, moved);
    CHECK(edit.count == 1);
    CHECK(edit.writes[0].field == HandleField::SustainRate);
    CHECK(edit.writes[0].value == 6);
  }
}

// ------------------------------------------------- what is not drawn at all

void test_the_release_dot_stands_where_the_release_reaches_the_floor() {
  // TL lifts the whole envelope, so the output saturates while the release
  // still has attenuation to cover: the trace ends after the graph does.
  const OperatorParams op = adsr(31, 10, 4, 5, 4, 31);
  const EnvelopeCurve curve = curve_of(op);
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandles handles = layout_of(curve);
  const EnvelopeHandle &release = handles.items[kReleaseHandle];
  CHECK(release.shown);

  double floor_ms = curve.release_content_ms;
  for (const auto &point : curve.release.points) {
    if (point.out >= kMaxAttenuation) {
      floor_ms = point.ms;
      break;
    }
  }
  CHECK(floor_ms < curve.release_content_ms);
  CHECK(near_abs(release.pos.x, plot.x_of(floor_ms), 0.51));
  // The solver is still asked about the release itself, and a drag of the dot
  // is worth that much more of it. The ratio is the two attenuations', which
  // the simulated ramp lands on to within a sample of its own.
  CHECK(near_abs(release.ms, curve.release_content_ms, 1e-6));
  CHECK_REL(release.ms_per_drawn, curve.release_content_ms / floor_ms, 0.001);
  CHECK(near_abs(dragged_ms(plot, release.ms, 0.0f, release.ms_per_drawn),
                 release.ms, 1e-9));
}

void test_a_line_handle_carries_the_end_it_is_pinned_at() {
  // A tilt is measured from where its phase begins: the release from full
  // volume at the origin, the sustain from the knee.
  const OperatorParams op = adsr(20, 12, 6, 8, 9, 24);
  const EnvelopeCurve curve = curve_of(op);
  const EnvelopeHandles handles = layout_of(curve);

  const EnvelopeHandle &release = handles.items[kReleaseHandle];
  CHECK(release.shown);
  CHECK(near_abs(release.anchor_ms, 0.0, 1e-9));
  CHECK(near_abs(release.anchor_out, curve.peak_out, 1e-9));

  const EnvelopeHandle &sustain = handles.items[kSustainHandle];
  if (sustain.shown) {
    CHECK(near_abs(sustain.anchor_ms, curve.decay_end_ms, 1e-9));
    CHECK(near_abs(sustain.anchor_out, curve.sustain_out, 1e-9));
  }

  // Tilting the release shallower asks for a slower fall, and a slower fall
  // is a smaller register.
  const double elapsed = release.at_ms;
  const double reach = elapsed * (kFullScale - release.anchor_out) /
                       (release.out - release.anchor_out);
  const int same =
      solve_release_rate(op, kMiddleC, reach * release.ms_per_drawn);
  CHECK(same == op.rr);
  const int slower =
      solve_release_rate(op, kMiddleC, reach * release.ms_per_drawn * 4.0);
  CHECK(slower < same);
}

void test_the_knee_stands_clear_of_the_peak_when_it_has_nowhere_to_be() {
  // A decay that never advances and one with nowhere to fall to both leave
  // the knee off the line; it waits a grab box and a dot clear of the peak,
  // which is far enough that the pointer can tell the two apart.
  const HandleMetrics m = metrics();
  for (const auto &op : {adsr(31, 0, 4, 5, 7, 20), adsr(31, 10, 0, 5, 7, 20)}) {
    const EnvelopeCurve curve = curve_of(op);
    const EnvelopeHandles handles = layout_of(curve);
    const EnvelopeHandle &knee = handles.items[kDecayHandle];
    CHECK(knee.shown);
    CHECK(knee.parked);
    const float clear = knee.pos.x - handles.items[kAttackHandle].pos.x;
    CHECK(clear >= m.grab);
    CHECK(nearest_handle(handles, knee.pos) == kDecayHandle);
    CHECK(nearest_handle(handles, handles.items[kAttackHandle].pos) ==
          kAttackHandle);
  }
}

void test_a_parked_handle_says_so() {
  // DR 0: the knee waits at the edge. SL 0: it waits beside the peak. Neither
  // stands where the curve would put it.
  const EnvelopeCurve never = curve_of(adsr(6, 0, 4, 0, 4, 31));
  CHECK(layout_of(never).items[kDecayHandle].parked);
  const EnvelopeCurve flat = curve_of(adsr(31, 10, 0, 5, 7, 20));
  const EnvelopeHandle &beside = layout_of(flat).items[kDecayHandle];
  CHECK(beside.shown);
  CHECK(beside.parked);
  // A knee the curve does put somewhere is not parked.
  const EnvelopeCurve real = curve_of(adsr(31, 10, 4, 5, 7, 20));
  CHECK(!layout_of(real).items[kDecayHandle].parked);
}

void test_the_pointer_answers_the_handle_it_is_nearest() {
  const EnvelopeCurve curve = curve_of(adsr(31, 10, 4, 5, 7, 20));
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandles handles = layout_of(curve);
  CHECK(handles.items[kAttackHandle].shown);
  CHECK(handles.items[kDecayHandle].shown);

  const Point peak = handles.items[kAttackHandle].pos;
  const Point knee = handles.items[kDecayHandle].pos;
  CHECK(nearest_handle(handles, peak) == kAttackHandle);
  CHECK(nearest_handle(handles, knee) == kDecayHandle);
  // Just off the peak, still its own.
  CHECK(nearest_handle(handles, Point{peak.x + 1.0f, peak.y}) == kAttackHandle);
  // Past every grab box, nobody answers.
  CHECK(nearest_handle(handles, Point{plot.max.x, plot.min.y}) == kHandleCount);
}

void test_a_sliver_of_sustain_still_carries_a_handle() {
  // A sustain level of 15 leaves the phase sixteen units tall, which is no
  // room to drag a line through -- but the dot stays, parked, because that is
  // where tilting one starts.
  const EnvelopeCurve curve = curve_of(adsr(31, 10, 15, 5, 7, 0));
  const EnvelopeHandles handles = layout_of(curve);
  CHECK(handles.items[kSustainHandle].shown);
  CHECK(handles.items[kSustainHandle].parked);
}

void test_a_release_that_outran_the_simulation_waits_at_the_edge() {
  // RR 0 falls for longer than the simulation covers, so its end is not on the
  // graph. The dot waits on the line at the right-hand edge rather than going
  // away: tilting it there is what brings the end back.
  const EnvelopeCurve curve = curve_of(adsr(31, 10, 4, 5, 0, 0));
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandle &release = layout_of(curve).items[kReleaseHandle];
  CHECK(release.shown);
  CHECK(release.parked);
  // It stands where the drawn line stops, which is where the simulation did.
  CHECK(near_abs(release.at_ms,
                 std::min(curve.release_content_ms, plot.span_ms), 1e-6));
}

void test_a_release_that_outran_its_budget_says_so() {
  // The release ends at the attenuation the chip cuts the output at, which TL
  // has nothing to do with: a loud patch's trace hits the bottom of the graph
  // long before the envelope behind it is done falling.
  for (const int tl : {0, 100}) {
    for (int rr = 0; rr <= 15; ++rr) {
      OperatorParams op = adsr(31, 10, 2, 5, rr, tl);
      const EnvelopeCurve curve = curve_of(op);
      CHECK(curve.release_truncated == (rr <= 3));
      // And the handle on it waits at the edge for exactly as long.
      CHECK(layout_of(curve).items[kReleaseHandle].parked == (rr <= 3));
    }
  }
}

// ------------------------------------------------------ back to a register

/**
 * The property the whole interaction rests on: asking a handle's own solver
 * about the place that handle already stands answers with a curve drawn in
 * exactly that place.
 *
 * Not always with the register it came from -- several rates can be the same
 * length, and every sustain level past the bottom of the scale is the bottom
 * of the scale -- but never with one that would move the curve out from under
 * the pointer.
 */
void test_reading_a_handle_back_leaves_the_curve_where_it_is() {
  for (int ks : {0, 3}) {
    for (int ar : {8, 20, 31}) {
      for (int dr : {4, 16, 28}) {
        for (int sl : {0, 3, 9, 14}) {
          for (int sr : {0, 7, 18, 31}) {
            for (int rr : {5, 11, 15}) {
              for (int tl : {0, 17, 64, 120}) {
                OperatorParams op = adsr(ar, dr, sl, sr, rr, tl);
                op.ks = static_cast<uint8_t>(ks);
                const EnvelopeCurve curve = curve_of(op);
                const PlotArea plot = plot_over(curve);
                const EnvelopeHandles handles = layout_of(curve);

                const EnvelopeHandle &attack = handles.items[kAttackHandle];
                if (attack.shown) {
                  CHECK(solved_curve(op, HandleField::AttackRate, attack.ms,
                                     0.0)
                            .attack_end_ms == curve.attack_end_ms);
                  // TL is the top seven bits of the level it sets, so it is
                  // the one field that always comes back as itself.
                  CHECK(solve_field(op, HandleField::TotalLevel, attack.out,
                                    0.0) == tl);
                }
                const EnvelopeHandle &decay = handles.items[kDecayHandle];
                if (decay.shown && !decay.parked) {
                  const EnvelopeCurve rate =
                      solved_curve(op, HandleField::DecayRate, decay.ms, 0.0);
                  CHECK(near_abs(rate.decay_end_ms - rate.attack_end_ms,
                                 decay.ms, 1e-6));
                  CHECK(solved_curve(op, HandleField::SustainLevel, decay.out,
                                     0.0)
                            .sustain_out == curve.sustain_out);
                }
                const EnvelopeHandle &sustain = handles.items[kSustainHandle];
                if (sustain.shown) {
                  const EnvelopeCurve rate =
                      solved_curve(op, HandleField::SustainRate, sustain.out,
                                   sustain.ms);
                  CHECK(near_abs(curve_out_at_ms(rate.held, sustain.at_ms),
                                 sustain.out, plot.out_per_px()));
                }
                const EnvelopeHandle &release = handles.items[kReleaseHandle];
                if (release.shown) {
                  CHECK(solved_curve(op, HandleField::ReleaseRate, release.ms,
                                     0.0)
                            .release_content_ms == release.ms);
                }
              }
            }
          }
        }
      }
    }
  }
}

/// Dragging one way makes the phase longer and the level quieter, all the way
/// to the ends of the ranges.
void test_a_drag_goes_where_it_is_pointed() {
  const OperatorParams op = adsr(20, 14, 6, 10, 8, 32);
  const EnvelopeCurve curve = curve_of(op);
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandles handles = layout_of(curve);

  const EnvelopeHandle &attack = handles.items[kAttackHandle];
  CHECK(solve_attack_rate(op, kMiddleC, dragged_ms(plot, attack.ms, 8.0f)) <
        20);
  CHECK(solve_attack_rate(op, kMiddleC, dragged_ms(plot, attack.ms, -8.0f)) >
        20);
  // Dragging the peak down is a quieter operator.
  CHECK(solve_total_level(dragged_out(plot, attack.out, 6.0f)) > 32);

  // Dragging the end of the release left is a quicker release, right a slower
  // one.
  const EnvelopeHandle &release = handles.items[kReleaseHandle];
  CHECK(solve_release_rate(op, kMiddleC, dragged_ms(plot, release.ms, -40.0f)) >
        8);
  CHECK(solve_release_rate(op, kMiddleC, dragged_ms(plot, release.ms, 40.0f)) <
        8);
  // RR 14 and 15 are the same length, so the quickest release a drag arrives
  // at is 14.
  CHECK(solve_release_rate(op, kMiddleC, 1.0) == 14);

  // The top of the sustain line is SR 0, the rate that holds.
  const EnvelopeHandle &sustain = handles.items[kSustainHandle];
  CHECK(solve_sustain_rate(op, kMiddleC, sustain.ms,
                           static_cast<double>(curve.sustain_out)) == 0);
}

// ------------------------------------------------ one pointer movement

void test_a_pointer_that_has_not_moved_writes_nothing() {
  // Every handle of every shape, including the ones waiting somewhere the
  // curve does not put them: a sustain level of 15 leaves a sliver, and a
  // decay with nowhere or no time to fall leaves no knee on the line at all.
  struct Patch {
    OperatorParams op;
    HandleIndex parked;
  };
  const Patch patches[] = {
      {worked_example(), kHandleCount},
      {adsr(31, 10, 15, 5, 7, 0), kSustainHandle},
      {adsr(31, 10, 0, 5, 7, 20), kDecayHandle},
      {adsr(6, 0, 4, 0, 4, 31), kDecayHandle},
  };
  for (const Patch &patch : patches) {
    const EnvelopeCurve curve = curve_of(patch.op);
    const EnvelopeHandles handles = layout_of(curve);
    if (patch.parked != kHandleCount) {
      CHECK(handles.items[patch.parked].shown);
      CHECK(handles.items[patch.parked].parked);
    }
    // kHandleCount included: a grab of nothing asks for nothing either.
    for (int i = 0; i <= kHandleCount; ++i) {
      const HandleGrab grab = grab_handle(handles, static_cast<HandleIndex>(i));
      CHECK(drag_handle(grab, patch.op, kMiddleC, Point{}).count == 0);
    }
  }
}

void test_a_knee_pulled_onto_the_peak_stops_the_decay() {
  // There is no decay left to describe, and the register for that is a decay
  // rate of 0. The sustain level stays one step below the peak so the knee has
  // somewhere to come back to.
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = curve_of(op);
  const EnvelopeHandles handles = layout_of(curve);
  const HandleGrab grab = grab_handle(handles, kDecayHandle);
  CHECK(grab.grabbed.shown);

  const HandleEdit edit =
      drag_handle(grab, op, kMiddleC, Point{4.0f, -100.0f});
  CHECK(edit.count == 2);
  CHECK(edit.writes[0].field == HandleField::SustainLevel);
  CHECK(edit.writes[0].value == 1);
  CHECK(edit.writes[1].field == HandleField::DecayRate);
  CHECK(edit.writes[1].value == 0);
}

void test_a_knee_dragged_down_solves_the_rate_against_the_new_level() {
  // How long a decay lasts is measured to where it is going, so the level goes
  // in first and the rate is solved against it.
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = curve_of(op);
  const PlotArea plot = plot_over(curve);
  const EnvelopeHandles handles = layout_of(curve);
  const HandleGrab grab = grab_handle(handles, kDecayHandle);
  CHECK(grab.grabbed.shown);

  const Point moved{20.0f, 30.0f};
  const HandleEdit edit = drag_handle(grab, op, kMiddleC, moved);
  CHECK(edit.count == 2);
  CHECK(edit.writes[0].field == HandleField::SustainLevel);
  CHECK(edit.writes[1].field == HandleField::DecayRate);
  CHECK(edit.writes[0].value > op.sl);

  const double at_ms = dragged_ms(plot, grab.grabbed.at_ms, moved.x);
  const double target =
      (at_ms - grab.grabbed.anchor_ms) * grab.grabbed.ms_per_drawn;
  const OperatorParams aimed =
      applied(op, HandleField::SustainLevel, edit.writes[0].value);
  CHECK(edit.writes[1].value == solve_decay_rate(aimed, kMiddleC, target));
  // The level it was grabbed at would have asked for another rate entirely.
  CHECK(edit.writes[1].value != solve_decay_rate(op, kMiddleC, target));
}

void test_a_release_tilted_level_or_upward_asks_for_the_slowest() {
  // The line is pinned at full volume, so a pointer at or above that height
  // tilts a line which never reaches the floor.
  const OperatorParams op = worked_example();
  const EnvelopeCurve curve = curve_of(op);
  const EnvelopeHandles handles = layout_of(curve);
  const HandleGrab grab = grab_handle(handles, kReleaseHandle);
  CHECK(grab.grabbed.shown);

  for (const float dy : {-100.0f, -140.0f}) {
    const HandleEdit edit = drag_handle(grab, op, kMiddleC, Point{0.0f, dy});
    CHECK(edit.count == 1);
    CHECK(edit.writes[0].field == HandleField::ReleaseRate);
    CHECK(edit.writes[0].value == 0);
  }
}

void test_dragging_the_peak_across_changes_the_attack_rate() {
  const OperatorParams op = adsr(20, 14, 6, 10, 8, 32);
  const EnvelopeCurve curve = curve_of(op);
  const EnvelopeHandles handles = layout_of(curve);
  const HandleGrab grab = grab_handle(handles, kAttackHandle);
  CHECK(grab.grabbed.shown);

  const HandleEdit slower =
      drag_handle(grab, op, kMiddleC, Point{8.0f, 0.0f});
  CHECK(slower.count == 1);
  CHECK(slower.writes[0].field == HandleField::AttackRate);
  CHECK(slower.writes[0].value < op.ar);

  const HandleEdit faster =
      drag_handle(grab, op, kMiddleC, Point{-8.0f, 0.0f});
  CHECK(faster.count == 1);
  CHECK(faster.writes[0].field == HandleField::AttackRate);
  CHECK(faster.writes[0].value > op.ar);

  // Straight down is the level alone.
  const HandleEdit quieter =
      drag_handle(grab, op, kMiddleC, Point{0.0f, 6.0f});
  CHECK(quieter.count == 1);
  CHECK(quieter.writes[0].field == HandleField::TotalLevel);
  CHECK(quieter.writes[0].value > op.tl);
}

} // namespace

void test_the_handle_and_field_values_are_fixed() {
  CHECK_EQ(static_cast<int>(kAttackHandle), 0);
  CHECK_EQ(static_cast<int>(kDecayHandle), 1);
  CHECK_EQ(static_cast<int>(kSustainHandle), 2);
  CHECK_EQ(static_cast<int>(kReleaseHandle), 3);
  CHECK_EQ(static_cast<int>(kHandleCount), 4);
  CHECK_EQ(static_cast<int>(HandleField::AttackRate), 0);
  CHECK_EQ(static_cast<int>(HandleField::DecayRate), 1);
  CHECK_EQ(static_cast<int>(HandleField::SustainLevel), 2);
  CHECK_EQ(static_cast<int>(HandleField::SustainRate), 3);
  CHECK_EQ(static_cast<int>(HandleField::ReleaseRate), 4);
  CHECK_EQ(static_cast<int>(HandleField::TotalLevel), 5);
}

void test_each_handle_names_the_registers_it_sets() {
  const HandleFields attack = handle_fields(kAttackHandle);
  CHECK(attack.across == HandleField::AttackRate);
  CHECK(attack.down == HandleField::TotalLevel);
  const HandleFields decay = handle_fields(kDecayHandle);
  CHECK(decay.across == HandleField::DecayRate);
  CHECK(decay.down == HandleField::SustainLevel);
  const HandleFields sustain = handle_fields(kSustainHandle);
  CHECK(!sustain.across);
  CHECK(sustain.down == HandleField::SustainRate);
  const HandleFields release = handle_fields(kReleaseHandle);
  CHECK(release.across == HandleField::ReleaseRate);
  CHECK(!release.down);
  const HandleFields none = handle_fields(kHandleCount);
  CHECK(!none.across && !none.down);
}

void test_a_drag_writes_only_the_registers_its_handle_names() {
  const Point moves[] = {{15.0f, 0.0f},  {-15.0f, 0.0f}, {0.0f, 12.0f},
                         {0.0f, -12.0f}, {20.0f, 20.0f}, {-30.0f, 40.0f},
                         {0.0f, -200.0f}};
  int writes = 0;
  for (const OperatorParams &op :
       {worked_example(), adsr(18, 6, 9, 3, 4, 20), adsr(31, 0, 5, 0, 15, 0),
        adsr(10, 20, 15, 12, 1, 60)}) {
    const EnvelopeHandles handles = layout_of(curve_of(op));
    for (int h = 0; h < kHandleCount; ++h) {
      const HandleIndex handle = static_cast<HandleIndex>(h);
      const HandleFields fields = handle_fields(handle);
      const HandleGrab grab = grab_handle(handles, handle);
      for (const Point moved : moves) {
        const HandleEdit edit = drag_handle(grab, op, kMiddleC, moved);
        for (int i = 0; i < edit.count; ++i) {
          const HandleField field =
              edit.writes[static_cast<std::size_t>(i)].field;
          CHECK(field == fields.across || field == fields.down);
          ++writes;
        }
      }
    }
  }
  CHECK(writes > 50);
}

int main() {
  std::cout << "handles_test\n";

  RUN_TEST(test_the_plot_maps_the_curve_onto_its_own_rectangle);
  RUN_TEST(test_a_drag_that_has_not_moved_asks_for_what_it_grabbed);

  RUN_TEST(test_every_handle_stands_where_the_curve_puts_it);
  RUN_TEST(test_the_whole_dot_stays_inside_the_plot);
  RUN_TEST(test_the_release_dot_stands_where_the_release_reaches_the_floor);
  RUN_TEST(test_a_line_handle_carries_the_end_it_is_pinned_at);

  RUN_TEST(test_every_ssg_dot_stands_on_the_line_it_edits);
  RUN_TEST(test_a_ramp_with_no_sustain_ends_in_its_decay);
  RUN_TEST(test_an_attack_past_the_axis_waits_at_its_edge);
  RUN_TEST(test_a_knee_pulled_onto_an_ssg_peak_keeps_the_loop_running);
  RUN_TEST(test_a_drag_to_another_values_dot_asks_for_that_value);

  RUN_TEST(test_the_knee_stands_clear_of_the_peak_when_it_has_nowhere_to_be);
  RUN_TEST(test_a_parked_handle_says_so);
  RUN_TEST(test_the_pointer_answers_the_handle_it_is_nearest);
  RUN_TEST(test_a_sliver_of_sustain_still_carries_a_handle);
  RUN_TEST(test_a_release_that_outran_the_simulation_waits_at_the_edge);
  RUN_TEST(test_a_release_that_outran_its_budget_says_so);

  RUN_TEST(test_reading_a_handle_back_leaves_the_curve_where_it_is);
  RUN_TEST(test_a_drag_goes_where_it_is_pointed);

  RUN_TEST(test_a_pointer_that_has_not_moved_writes_nothing);
  RUN_TEST(test_a_knee_pulled_onto_the_peak_stops_the_decay);
  RUN_TEST(test_a_knee_dragged_down_solves_the_rate_against_the_new_level);
  RUN_TEST(test_a_release_tilted_level_or_upward_asks_for_the_slowest);
  RUN_TEST(test_dragging_the_peak_across_changes_the_attack_rate);

  RUN_TEST(test_the_handle_and_field_values_are_fixed);
  RUN_TEST(test_each_handle_names_the_registers_it_sets);
  RUN_TEST(test_a_drag_writes_only_the_registers_its_handle_names);

  return testing::summary();
}
