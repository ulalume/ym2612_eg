// sample_curve(): markers, park/loop detection, warnings, decimation.

#include "check.hpp"
#include "helpers.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

using namespace ym2612_eg;

namespace {

constexpr NotePitch kC4{644, 4};
constexpr NotePitch kRks0{644, 0};

bool has_marker(const CurveResult &r, MarkerKind kind) {
  for (const Marker &m : r.markers)
    if (m.kind == kind)
      return true;
  return false;
}

double marker_ms(const CurveResult &r, MarkerKind kind) {
  for (const Marker &m : r.markers)
    if (m.kind == kind)
      return m.ms;
  return -1.0;
}

size_t count_marker(const CurveResult &r, MarkerKind kind) {
  size_t n = 0;
  for (const Marker &m : r.markers)
    if (m.kind == kind)
      ++n;
  return n;
}

bool has_warning(const CurveResult &r, CurveWarning w) {
  return std::find(r.warnings.begin(), r.warnings.end(), w) !=
         r.warnings.end();
}

// Linear interpolation of a decimated curve at an arbitrary time.
void eval_curve(const std::vector<CurvePoint> &pts, double ms, double &out,
                double &att) {
  if (pts.empty()) {
    out = att = 0.0;
    return;
  }
  if (ms <= pts.front().ms) {
    out = pts.front().out;
    att = pts.front().att;
    return;
  }
  if (ms >= pts.back().ms) {
    out = pts.back().out;
    att = pts.back().att;
    return;
  }
  size_t lo = 0, hi = pts.size() - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (pts[mid].ms <= ms)
      lo = mid;
    else
      hi = mid;
  }
  const double span = static_cast<double>(pts[hi].ms) - pts[lo].ms;
  const double u = span > 0.0 ? (ms - pts[lo].ms) / span : 0.0;
  out = pts[lo].out + (static_cast<double>(pts[hi].out) - pts[lo].out) * u;
  att = pts[lo].att + (static_cast<double>(pts[hi].att) - pts[lo].att) * u;
}

// Rebuild the undecimated change-point polyline for a held-forever request.
std::vector<CurvePoint> raw_curve(const CurveRequest &req, double end_ms) {
  EgSimulator sim(req.op, req.pitch, req.clock_hz);
  sim.reset(0, req.start_att);
  sim.key_on();
  std::vector<CurvePoint> raw;
  raw.push_back(CurvePoint{0.0f, sim.output(), sim.attenuation()});
  while (sim.time_ms() < end_ms) {
    sim.step();
    const uint16_t o = sim.output();
    const uint16_t a = sim.attenuation();
    if (raw.back().out != o || raw.back().att != a)
      raw.push_back(
          CurvePoint{static_cast<float>(sim.time_ms()), o, a});
  }
  return raw;
}

// ---------------------------------------------------------------------------

void test_points_and_marker_ordering() {
  CurveRequest req;
  req.op = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0};
  req.pitch = kC4;
  req.gate_ms = 500.0;
  req.max_ms = 2500.0;

  const CurveResult r = sample_curve(req);
  CHECK(!r.points.empty());
  CHECK_EQ(r.points.front().ms, 0.0f);
  CHECK_EQ(r.points.front().att, 0); // instant attack

  // Points are strictly ordered in time.
  bool ordered = true;
  for (size_t i = 1; i < r.points.size(); ++i)
    if (!(r.points[i].ms >= r.points[i - 1].ms))
      ordered = false;
  CHECK(ordered);

  // Markers are ordered too.
  bool m_ordered = true;
  for (size_t i = 1; i < r.markers.size(); ++i)
    if (!(r.markers[i].ms >= r.markers[i - 1].ms))
      m_ordered = false;
  CHECK(m_ordered);

  CHECK(has_marker(r, MarkerKind::AttackEnd));
  CHECK(has_marker(r, MarkerKind::DecayEnd));
  CHECK(has_marker(r, MarkerKind::KeyOff));
  CHECK(has_marker(r, MarkerKind::Park));
  CHECK(has_marker(r, MarkerKind::Silence));
  CHECK(!has_marker(r, MarkerKind::SsgFold));

  // Fires on EG tick 1, i.e. the very first output sample.
  CHECK(marker_ms(r, MarkerKind::AttackEnd) > 0.0);
  CHECK(marker_ms(r, MarkerKind::AttackEnd) <= 0.0564);
  CHECK_REL(marker_ms(r, MarkerKind::DecayEnd), 306.4, 0.01);
  CHECK_REL(marker_ms(r, MarkerKind::KeyOff), 500.0, 0.01);
  // Release from the sustain level takes ~849 ms after key-off.
  CHECK_REL(marker_ms(r, MarkerKind::Park), 500.0 + 849.0, 0.02);
  CHECK_REL(r.park_ms, marker_ms(r, MarkerKind::Park), 1e-4);
  CHECK(std::isfinite(r.park_ms));
  CHECK(marker_ms(r, MarkerKind::Silence) <= r.park_ms);
  CHECK_EQ(r.loop_hz, 0.0);
  CHECK(r.warnings.empty());
  // Simulation stopped at the park, well before max_ms.
  CHECK(r.points.back().ms < 1500.0f);
}

void test_park_when_held_forever() {
  CurveRequest req;
  req.op = OperatorParams{31, 10, 0, 7, 2, 0, 0, 0}; // SR = 0 -> holds
  req.pitch = kC4;
  req.gate_ms = -1.0;
  req.max_ms = 5000.0;

  const CurveResult r = sample_curve(req);
  CHECK(std::isfinite(r.park_ms));
  CHECK_REL(r.park_ms, 306.4, 0.02);
  CHECK(has_marker(r, MarkerKind::Park));
  CHECK(!has_marker(r, MarkerKind::KeyOff));
  CHECK(!has_marker(r, MarkerKind::Silence)); // parked at -6 dB, audible
  CHECK(r.points.back().att == 64);
}

void test_never_parks_within_max_ms() {
  CurveRequest req;
  req.op = OperatorParams{31, 10, 1, 7, 2, 0, 0, 0}; // very slow second decay
  req.pitch = kC4;
  req.gate_ms = -1.0;
  req.max_ms = 200.0;
  const CurveResult r = sample_curve(req);
  CHECK(!std::isfinite(r.park_ms));
  CHECK(!has_marker(r, MarkerKind::Park));
  CHECK_ABS(r.points.back().ms, 200.0, 1.0);
}

void test_ssg_loop_detection() {
  CurveRequest req;
  req.op = OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x08};
  req.pitch = kRks0;
  req.gate_ms = -1.0;
  req.max_ms = 5000.0;

  const CurveResult r = sample_curve(req);
  CHECK_REL(r.loop_hz, 6.51, 0.01);
  CHECK(!std::isfinite(r.park_ms));
  CHECK(has_marker(r, MarkerKind::SsgFold));
  CHECK(has_marker(r, MarkerKind::SsgPhaseReset)); // mode 0 resets the PG
  CHECK(!has_marker(r, MarkerKind::SsgInvert));
  CHECK(!has_marker(r, MarkerKind::SsgHold));
  CHECK(r.warnings.empty());
  // Stops after five full loops, not at max_ms.
  CHECK_REL(r.points.back().ms, 5 * 153.7, 0.02);
  CHECK_EQ(count_marker(r, MarkerKind::SsgFold), size_t{5});
}

void test_ssg_alternate_counts_two_ramps() {
  CurveRequest req;
  req.op = OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x0A}; // mode 2, triangle
  req.pitch = kRks0;
  req.gate_ms = -1.0;
  req.max_ms = 5000.0;

  const CurveResult r = sample_curve(req);
  // Two ramps per visible period -> half the mode-0 rate.
  CHECK_REL(r.loop_hz, 6.51 / 2.0, 0.01);
  CHECK(has_marker(r, MarkerKind::SsgInvert));
  CHECK(!has_marker(r, MarkerKind::SsgPhaseReset)); // alternate set
  CHECK_EQ(count_marker(r, MarkerKind::SsgFold), size_t{10});
}

void test_ssg_hold_parks() {
  CurveRequest req;
  req.op = OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x09}; // mode 1
  req.pitch = kRks0;
  req.gate_ms = -1.0;
  req.max_ms = 5000.0;

  const CurveResult r = sample_curve(req);
  CHECK_EQ(r.loop_hz, 0.0);
  CHECK(std::isfinite(r.park_ms));
  CHECK(has_marker(r, MarkerKind::SsgHold));
  CHECK(has_marker(r, MarkerKind::Silence));
  CHECK_REL(r.park_ms, 153.7, 0.05);
  CHECK_EQ(r.points.back().att, 0x3FF);

  // Mode 5 holds at full volume instead: output 0, attenuation frozen at 0x200.
  CurveRequest req5 = req;
  req5.op.ssg = 0x0D;
  const CurveResult r5 = sample_curve(req5);
  CHECK(has_marker(r5, MarkerKind::SsgHold));
  CHECK(!has_marker(r5, MarkerKind::Silence));
  CHECK_EQ(r5.points.back().att, 0x200);
  CHECK_EQ(r5.points.back().out, 0);
  CHECK(std::isfinite(r5.park_ms));
}

void test_warnings() {
  // AR = 0: the operator can never attack.
  {
    CurveRequest req;
    req.op = OperatorParams{0, 10, 5, 7, 2, 0, 0, 0};
    req.pitch = kC4;
    req.gate_ms = -1.0;
    req.max_ms = 500.0;
    const CurveResult r = sample_curve(req);
    CHECK(has_warning(r, CurveWarning::AttackFrozen));
    CHECK(std::isfinite(r.park_ms));
  }
  // SSG with AR < 31 leaves the documented use case.
  {
    CurveRequest req;
    req.op = OperatorParams{20, 15, 0, 15, 15, 0, 0, 0x08};
    req.pitch = kRks0;
    req.gate_ms = -1.0;
    req.max_ms = 1000.0;
    const CurveResult r = sample_curve(req);
    CHECK(has_warning(r, CurveWarning::SsgArBelow31));
  }
  // SR = 0 with SL <= 14 in a looping mode: nothing ever oscillates.
  {
    CurveRequest req;
    req.op = OperatorParams{31, 31, 0, 15, 4, 0, 0, 0x08};
    req.pitch = kRks0;
    req.gate_ms = -1.0;
    req.max_ms = 1000.0;
    const CurveResult r = sample_curve(req);
    CHECK(has_warning(r, CurveWarning::SsgNeverLoops));
    CHECK_EQ(r.loop_hz, 0.0);
    CHECK(std::isfinite(r.park_ms));
  }
  // DR = 0 with SL > 0: same story from the other side.
  {
    CurveRequest req;
    req.op = OperatorParams{31, 0, 31, 15, 4, 0, 0, 0x08};
    req.pitch = kRks0;
    req.gate_ms = -1.0;
    req.max_ms = 1000.0;
    const CurveResult r = sample_curve(req);
    CHECK(has_warning(r, CurveWarning::SsgNeverLoops));
  }
  // Above ~20 Hz the loop stops being an envelope.
  {
    CurveRequest req;
    req.op = OperatorParams{31, 28, 0, 15, 15, 0, 0, 0x08};
    req.pitch = kRks0;
    req.gate_ms = -1.0;
    req.max_ms = 1000.0;
    const CurveResult r = sample_curve(req);
    CHECK_REL(r.loop_hz, 554.9, 0.01);
    CHECK(has_warning(r, CurveWarning::SsgAudioRate));
  }
  // A hold mode is not a looping mode, so it never warns about looping.
  {
    CurveRequest req;
    req.op = OperatorParams{31, 31, 0, 15, 4, 0, 0, 0x09};
    req.pitch = kRks0;
    req.gate_ms = -1.0;
    req.max_ms = 1000.0;
    const CurveResult r = sample_curve(req);
    CHECK(!has_warning(r, CurveWarning::SsgNeverLoops));
  }
}

void test_start_att_retrigger() {
  CurveRequest req;
  req.op = OperatorParams{16, 10, 5, 7, 2, 0, 0, 0}; // AR=16, not instant
  req.pitch = kC4;
  req.gate_ms = -1.0;
  req.max_ms = 500.0;
  req.start_att = 512;
  const CurveResult r = sample_curve(req);
  CHECK_EQ(r.points.front().att, 512);
  CHECK(r.points.size() > 2);
  CHECK(r.points[1].att < 512); // attack resumes from there
}

void test_ssg_key_off_segment() {
  CurveRequest req;
  req.op = OperatorParams{31, 15, 0, 8, 15, 0, 0, 0x0A}; // mode 2
  req.pitch = kRks0;
  req.gate_ms = 200.0;
  req.max_ms = 2000.0;
  const CurveResult r = sample_curve(req);
  CHECK(has_marker(r, MarkerKind::KeyOff));
  CHECK(has_marker(r, MarkerKind::SsgInvert));
  CHECK(std::isfinite(r.park_ms));
  CHECK(r.park_ms > 200.0);
  CHECK_EQ(r.points.back().att, 0x3FF); // hard cut at 0x200
  CHECK(has_marker(r, MarkerKind::Silence));
}

void test_decimation_fidelity() {
  struct Case {
    OperatorParams op;
    NotePitch pitch;
    double max_ms;
    double min_reduction; // raw / decimated
  };
  // The SSG cases step attenuation by 4 at a time, so their staircase really
  // does deviate ~3 units from a straight line and cannot be flattened much
  // without breaking the 1-unit guarantee.
  const Case cases[] = {
      {OperatorParams{31, 10, 5, 7, 2, 0, 0, 0}, kC4, 3000.0, 10.0},
      {OperatorParams{16, 12, 3, 9, 6, 20, 1, 0}, kC4, 3000.0, 5.0},
      {OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x08}, kRks0, 3000.0, 1.2},
      {OperatorParams{31, 20, 0, 15, 15, 8, 0, 0x0A}, kRks0, 3000.0, 5.0},
  };

  for (const Case &c : cases) {
    CurveRequest req;
    req.op = c.op;
    req.pitch = c.pitch;
    req.gate_ms = -1.0;
    req.max_ms = c.max_ms;
    const CurveResult r = sample_curve(req);
    CHECK(r.points.size() >= 2);

    const double end_ms = r.points.back().ms;
    const std::vector<CurvePoint> raw = raw_curve(req, end_ms);
    CHECK(raw.size() >= r.points.size());

    double worst_out = 0.0, worst_att = 0.0;
    for (const CurvePoint &p : raw) {
      if (p.ms > end_ms)
        break;
      double out = 0.0, att = 0.0;
      eval_curve(r.points, p.ms, out, att);
      worst_out = std::max(worst_out, std::fabs(out - p.out));
      worst_att = std::max(worst_att, std::fabs(att - p.att));
    }
    CHECK(worst_out < 1.0);
    CHECK(worst_att < 1.0);
    // The decimation has to actually do something.
    CHECK(static_cast<double>(raw.size()) /
              static_cast<double>(r.points.size()) >=
          c.min_reduction);
  }
}

void test_marker_points_are_kept() {
  CurveRequest req;
  req.op = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0};
  req.pitch = kC4;
  req.gate_ms = 400.0;
  req.max_ms = 2500.0;
  const CurveResult r = sample_curve(req);
  for (const Marker &m : r.markers) {
    bool found = false;
    for (const CurvePoint &p : r.points)
      if (std::fabs(static_cast<double>(p.ms) - m.ms) < 1e-4) {
        found = true;
        break;
      }
    CHECK(found);
  }
}

void test_clock_selection() {
  CurveRequest ntsc;
  ntsc.op = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0};
  ntsc.pitch = kC4;
  ntsc.gate_ms = -1.0;
  ntsc.max_ms = 1000.0;
  CurveRequest pal = ntsc;
  pal.clock_hz = kPalClockHz;

  const double a = marker_ms(sample_curve(ntsc), MarkerKind::DecayEnd);
  const double b = marker_ms(sample_curve(pal), MarkerKind::DecayEnd);
  CHECK_REL(b / a, kNtscClockHz / kPalClockHz, 0.001);
}

// Regression: a curve that parks during sustain hold (SR=0) must still get
// its release segment after the gate ends.  The park detector re-arms at
// key-off; park_ms stays at the first (sustain-hold) park.
void test_park_then_key_off_keeps_release() {
  CurveRequest req;
  req.op = OperatorParams{31, 31, 0, 7, 4, 0, 0, 0}; // instant decay, SR=0
  req.pitch = kC4;
  req.gate_ms = 500.0;
  req.max_ms = 3000.0;
  const CurveResult r = sample_curve(req);

  CHECK(r.park_ms < 500.0);              // first park = sustain hold
  CHECK(count_marker(r, MarkerKind::Park) == 2); // re-park at end of release
  CHECK(marker_ms(r, MarkerKind::KeyOff) >= 500.0 - 0.1);
  CHECK(r.points.back().ms > 600.0);     // release actually simulated
  CHECK(r.points.back().att == kMaxAttenuation);
}

// An SSG-EG alternate mode with AR < 31 inverts on every sample, so the output
// genuinely has a value per sample and decimation cannot thin it. The vertex
// count must still be bounded, and the extremes must survive so the band is
// visible.
void test_a_sample_rate_oscillation_stays_bounded() {
  CurveRequest req;
  req.op = OperatorParams{0, 3, 0, 7, 4, 0, 3, 0x0E}; // AR=0, SSG type 6
  req.pitch = kC4;
  req.gate_ms = -1.0;
  req.max_ms = 12000.0;
  const CurveResult r = sample_curve(req);

  CHECK(r.points.size() <= detail::kMaxPoints + 2);
  CHECK(r.points.size() > 16);

  // Both rails of the oscillation are still represented.
  uint16_t lowest = kMaxAttenuation;
  uint16_t highest = 0;
  for (const CurvePoint &p : r.points) {
    lowest = p.out < lowest ? p.out : lowest;
    highest = p.out > highest ? p.out : highest;
  }
  CHECK(highest - lowest > 256);
}

// A held patch with SR = 0 parks at its sustain level -- but the Decay ->
// Sustain transition must be reported before it does, or a caller loses the
// decay's end and with it the boundary between two phases.
void test_parking_at_the_sustain_level_still_reports_the_decay() {
  CurveRequest req;
  req.op = OperatorParams{31, 10, 0, 7, 4, 0, 0, 0}; // SR = 0 -> holds at SL
  req.pitch = kC4;
  req.gate_ms = -1.0;
  req.max_ms = 5000.0;
  const CurveResult r = sample_curve(req);

  CHECK(has_marker(r, MarkerKind::DecayEnd));
  CHECK(has_marker(r, MarkerKind::Park));
  CHECK(marker_ms(r, MarkerKind::DecayEnd) <= marker_ms(r, MarkerKind::Park));
  CHECK(std::isfinite(r.park_ms));
  // The park still follows the decay by a tick, not by a phase.
  CHECK_ABS(r.park_ms, marker_ms(r, MarkerKind::DecayEnd), 1.0);
}

} // namespace

int main() {
  std::cout << "curve_test\n";
  RUN_TEST(test_points_and_marker_ordering);
  RUN_TEST(test_park_then_key_off_keeps_release);
  RUN_TEST(test_park_when_held_forever);
  RUN_TEST(test_parking_at_the_sustain_level_still_reports_the_decay);
  RUN_TEST(test_never_parks_within_max_ms);
  RUN_TEST(test_ssg_loop_detection);
  RUN_TEST(test_ssg_alternate_counts_two_ramps);
  RUN_TEST(test_ssg_hold_parks);
  RUN_TEST(test_warnings);
  RUN_TEST(test_start_att_retrigger);
  RUN_TEST(test_ssg_key_off_segment);
  RUN_TEST(test_decimation_fidelity);
  RUN_TEST(test_marker_points_are_kept);
  RUN_TEST(test_clock_selection);
  RUN_TEST(test_a_sample_rate_oscillation_stays_bounded);
  return testing::summary();
}
