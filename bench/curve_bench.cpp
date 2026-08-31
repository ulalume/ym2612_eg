// Timing for sample_curve() on a 10 second span.

#include "ym2612_eg/ym2612_eg.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace ym2612_eg;

namespace {

double run(const char *name, const CurveRequest &req, int iterations) {
  CurveResult r;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i)
    r = sample_curve(req);
  const auto t1 = std::chrono::steady_clock::now();
  const double ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
  std::printf("%-28s %8.3f ms   points=%-7zu markers=%-5zu span=%.1f ms "
              "loop=%.2f Hz\n",
              name, ms, r.points.size(), r.markers.size(),
              r.points.empty() ? 0.0 : static_cast<double>(r.points.back().ms),
              r.loop_hz);
  return ms;
}

} // namespace

int main() {
  // 10 s of SSG looping: gate == max keeps the operator keyed on for the whole
  // span, so the five-loop early-out never fires.
  CurveRequest ssg;
  ssg.op = OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x08};
  ssg.pitch = NotePitch{644, 0};
  ssg.gate_ms = 10000.0;
  ssg.max_ms = 10000.0;
  run("10 s SSG loop (6.5 Hz)", ssg, 10);

  CurveRequest fast = ssg;
  fast.op.dr = 24; // 139 Hz loop
  run("10 s SSG loop (139 Hz)", fast, 10);

  CurveRequest plain;
  plain.op = OperatorParams{31, 10, 1, 7, 2, 0, 0, 0};
  plain.pitch = NotePitch{644, 4};
  plain.gate_ms = 10000.0;
  plain.max_ms = 10000.0;
  run("10 s plain ADSR", plain, 10);

  // The alternate fold squares the output at the sample rate with a stalled
  // attack behind it, so the whole axis is one band.
  CurveRequest band2;
  band2.op = OperatorParams{0, 0, 0, 15, 0, 0, 0, 0x0A};
  band2.pitch = NotePitch{644, 0};
  band2.gate_ms = 10000.0;
  band2.max_ms = 10000.0;
  run("10 s SSG band (type 2)", band2, 10);

  CurveRequest band6 = band2;
  band6.op = OperatorParams{0, 20, 14, 15, 0, 0, 0, 0x0E};
  run("10 s SSG band (type 6)", band6, 10);

  CurveRequest held;
  held.op = OperatorParams{31, 15, 0, 15, 15, 0, 0, 0x08};
  held.pitch = NotePitch{644, 0};
  held.gate_ms = -1.0;
  held.max_ms = 10000.0;
  run("held SSG (5 loop early-out)", held, 10);
  return 0;
}
