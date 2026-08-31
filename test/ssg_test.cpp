// SSG-EG: every numeric anchor and behavioural rule in SSG_EG_SPEC.md.

#include "check.hpp"
#include "helpers.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

using namespace ym2612_eg;
using helpers::eg_tick;
using helpers::mean_ramp_ms;
using helpers::ssg_fold_samples;
using helpers::ticks_until;

namespace {

// Block 0 keeps keycode < 8, so ksv (Rks) = 0 at KS = 0.
constexpr NotePitch kRks0{644, 0};
// Block 4 gives keycode 16, so ksv = 2 at KS = 0.
constexpr NotePitch kRks2{644, 4};

OperatorParams ssg_patch(int dr, int sr, int sl, int ssg, int ar = 31,
                         int rr = 15) {
  OperatorParams op{};
  op.ar = static_cast<uint8_t>(ar);
  op.dr = static_cast<uint8_t>(dr);
  op.sr = static_cast<uint8_t>(sr);
  op.rr = static_cast<uint8_t>(rr);
  op.sl = static_cast<uint8_t>(sl);
  op.ssg = static_cast<uint8_t>(ssg);
  return op;
}

// ---------------------------------------------------------------- section 1

void test_register_bit_decode() {
  // J = attack XOR invert; the invert flag is 0 right after key-on, so J
  // starts equal to the attack bit -- and is 0 whenever SSG is disabled.
  for (int mode = 0; mode < 8; ++mode) {
    EgSimulator on(ssg_patch(15, 0, 15, 0x08 | mode), kRks0);
    on.key_on();
    CHECK_EQ(on.ssg_inverted(), (mode & 0x04) != 0);
    EgSimulator off(ssg_patch(15, 0, 15, mode), kRks0);
    off.key_on();
    CHECK_EQ(off.ssg_inverted(), false);
  }
  // While keyed off, inversion is never *applied* on read even though the
  // effective J stays set (SSG_EG_SPEC 2c).
  EgSimulator s(ssg_patch(15, 0, 15, 0x0C), kRks0);
  CHECK_EQ(s.ssg_inverted(), true);
  CHECK_EQ(s.output(), s.attenuation());
  s.key_on();
  s.step(3000);
  CHECK(s.attenuation() > 0 && s.attenuation() < kSsgFoldAttenuation);
  CHECK_EQ(s.output(), static_cast<uint16_t>(0x200 - s.attenuation()));
}

// ---------------------------------------------------------------- section 6

void test_loop_period_vs_dr() {
  // SSG_EG_SPEC section 6: mode 0b1000, AR=31, SL=15, Rks=0.
  struct Case {
    int dr, rate;
    double ms;
  };
  const Case cases[] = {{8, 16, 1845.0},  {10, 20, 922.5},  {12, 24, 461.3},
                        {14, 28, 230.6},  {15, 30, 153.7},  {16, 32, 115.3},
                        {18, 36, 57.66},  {20, 40, 28.84},  {24, 48, 7.209},
                        {28, 56, 1.802},  {31, 62, 0.901}};
  for (const Case &c : cases) {
    EgSimulator sim(ssg_patch(c.dr, 0, 15, 0x08), kRks0);
    CHECK_EQ(sim.rate_of(EgPhase::Decay), c.rate);
    CHECK_EQ(sim.sustain_attenuation(), 0x3E0); // never reached: DR governs
    sim.key_on();
    const auto folds = ssg_fold_samples(sim, 7, 900000);
    CHECK_EQ(folds.size(), size_t{7});
    CHECK_REL(mean_ramp_ms(folds), c.ms, 0.01);
  }
}

void test_loop_period_key_scaled() {
  // Same DR=15 patch one keycode group up: Rks=2 -> rate 32 -> 115.3 ms.
  EgSimulator sim(ssg_patch(15, 0, 15, 0x08), kRks2);
  CHECK_EQ(sim.key_scale_value(), 2);
  CHECK_EQ(sim.rate_of(EgPhase::Decay), 32);
  sim.key_on();
  const auto folds = ssg_fold_samples(sim, 7, 200000);
  CHECK_EQ(folds.size(), size_t{7});
  CHECK_REL(mean_ramp_ms(folds), 115.31, 0.01);
}

void test_loop_period_vs_sr() {
  // SSG_EG_SPEC section 6 sanity anchors: SL=4, DR=31, AR=31, Rks=0.
  struct Case {
    int sr;
    double ms;
  };
  const Case cases[] = {{10, 691.9}, {15, 115.3}, {20, 21.85}, {31, 0.901}};
  for (const Case &c : cases) {
    EgSimulator sim(ssg_patch(31, c.sr, 4, 0x08), kRks0);
    CHECK_EQ(sim.sustain_attenuation(), 128);
    sim.key_on();
    const auto folds = ssg_fold_samples(sim, 7, 900000);
    CHECK_EQ(folds.size(), size_t{7});
    CHECK_REL(mean_ramp_ms(folds), c.ms, 0.01);
  }
}

void test_sr0_never_loops() {
  // SL <= 14 with SR = 0: the ramp parks at SL and never reaches 0x200.
  EgSimulator sim(ssg_patch(31, 0, 4, 0x08), kRks0);
  sim.key_on();
  const auto folds = ssg_fold_samples(sim, 3, 400000);
  CHECK_EQ(folds.size(), size_t{0});
  CHECK_EQ(sim.attenuation(), 128); // parked exactly at the sustain level
  CHECK(sim.phase() == EgPhase::Sustain);
  CHECK(sim.is_static());
}

void test_dr0_never_loops() {
  // DR = 0 with SL > 0: decay never advances, so the operator sits at 0.
  EgSimulator sim(ssg_patch(0, 31, 4, 0x08), kRks0);
  sim.key_on();
  const auto folds = ssg_fold_samples(sim, 3, 400000);
  CHECK_EQ(folds.size(), size_t{0});
  CHECK_EQ(sim.attenuation(), 0);
  CHECK(sim.is_static());
}

// ---------------------------------------------------------------- section 2

void test_one_sample_at_0x200_before_fold() {
  EgSimulator sim(ssg_patch(15, 0, 15, 0x08), kRks0);
  sim.key_on();
  int runs = 0;
  int current = 0;
  int max_run = 0;
  for (uint64_t i = 0; i < 200000 && runs < 4; ++i) {
    if (sim.attenuation() >= kSsgFoldAttenuation) {
      // The ramp lands exactly on 0x200 with these increments.
      CHECK_EQ(sim.attenuation(), 0x200);
      ++current;
    } else if (current > 0) {
      max_run = std::max(max_run, current);
      current = 0;
      ++runs;
    }
    sim.step();
  }
  CHECK_EQ(runs, 4);
  CHECK_EQ(max_run, 1); // exactly one sample is emitted at 0x200
}

void test_four_times_increment_and_freeze() {
  // Decay increments are 4x, and stop dead at 0x200 (they do not run on to
  // 0x3FF as they would without SSG).
  EgSimulator with(ssg_patch(15, 0, 15, 0x08), kRks0);
  EgSimulator without(ssg_patch(15, 0, 15, 0x00), kRks0);
  with.key_on();
  without.key_on();
  int compared = 0;
  for (int t = 0; t < 4000; ++t) {
    eg_tick(with);
    eg_tick(without);
    // Judge the fold by the non-SSG twin: `with` is folded back to 0 by the
    // SSG block on the sample right after it lands on 0x200.
    if (without.attenuation() * 4 >= kSsgFoldAttenuation)
      break;
    CHECK_EQ(with.attenuation(), without.attenuation() * 4);
    ++compared;
  }
  CHECK(compared > 500);

  // The freeze: attenuation never rises above 0x200 in a keyed-on SSG decay,
  // where the plain envelope would run all the way to 0x3FF.
  EgSimulator loop(ssg_patch(15, 0, 15, 0x08), kRks0);
  loop.key_on();
  uint16_t peak = 0;
  for (uint64_t i = 0; i < 100000; ++i) {
    peak = std::max(peak, loop.attenuation());
    loop.step();
  }
  CHECK_EQ(peak, 0x200);
}

void test_attack_phase_unaffected_by_ssg() {
  // SSG changes nothing inside Attack (SSG_EG_SPEC section 2a / 5).
  OperatorParams a = ssg_patch(15, 0, 15, 0x08, /*ar=*/16);
  OperatorParams b = a;
  b.ssg = 0;
  EgSimulator with(a, kRks2), without(b, kRks2);
  with.key_on();
  without.key_on();
  CHECK_EQ(with.attenuation(), 0x3FF);
  bool same = true;
  for (int t = 0; t < 2000; ++t) {
    eg_tick(with);
    eg_tick(without);
    if (with.phase() != EgPhase::Attack && without.phase() != EgPhase::Attack)
      break;
    if (with.attenuation() != without.attenuation())
      same = false;
  }
  CHECK(same);
}

// ---------------------------------------------------------------- section 1
// The eight output shapes.

void test_mode_shapes() {
  auto run = [](int mode, uint64_t samples) {
    EgSimulator sim(ssg_patch(15, 0, 15, 0x08 | mode), kRks0);
    sim.key_on();
    sim.step(static_cast<uint32_t>(samples));
    return sim;
  };
  const uint32_t one_ramp = 8200; // ~154 ms at 53267 Hz

  // Mode 0: repeating decay, never inverted, phase generator reset each cycle.
  {
    EgSimulator sim = run(0, one_ramp * 3);
    const auto folds = ssg_fold_samples(sim, 4, 200000);
    CHECK_EQ(folds.size(), size_t{4});
    CHECK(!sim.ssg_inverted());
    CHECK(!sim.is_static());
  }
  // Mode 1: one decay, then jump to 0x3FF and hold.
  {
    EgSimulator sim = run(1, one_ramp * 2);
    CHECK_EQ(sim.attenuation(), 0x3FF);
    CHECK_EQ(sim.output(), 0x3FF);
    CHECK(sim.is_static());
  }
  // Mode 2: triangle -- inversion toggles at every fold.
  {
    EgSimulator sim(ssg_patch(15, 0, 15, 0x0A), kRks0);
    sim.key_on();
    CHECK(!sim.ssg_inverted());
    ssg_fold_samples(sim, 1, 200000);
    sim.step(); // consume the fold
    CHECK(sim.ssg_inverted());
    ssg_fold_samples(sim, 1, 200000);
    sim.step();
    CHECK(!sim.ssg_inverted());
    CHECK(!sim.is_static());
  }
  // Mode 3: one decay, then full volume, held (A frozen at 0x200, output 0).
  {
    EgSimulator sim = run(3, one_ramp * 2);
    CHECK_EQ(sim.attenuation(), 0x200);
    CHECK(sim.ssg_inverted());
    CHECK_EQ(sim.output(), 0);
    CHECK(sim.is_static());
  }
  // Mode 4: repeating rise (always inverted), loops like mode 0.
  {
    EgSimulator sim = run(4, one_ramp * 3);
    CHECK(sim.ssg_inverted());
    const auto folds = ssg_fold_samples(sim, 4, 200000);
    CHECK_EQ(folds.size(), size_t{4});
    CHECK(!sim.is_static());
  }
  // Mode 5: one rise, then hold at full volume.
  {
    EgSimulator sim = run(5, one_ramp * 2);
    CHECK_EQ(sim.attenuation(), 0x200);
    CHECK(sim.ssg_inverted());
    CHECK_EQ(sim.output(), 0);
    CHECK(sim.is_static());
  }
  // Mode 6: triangle starting with a rise.
  {
    EgSimulator sim(ssg_patch(15, 0, 15, 0x0E), kRks0);
    sim.key_on();
    CHECK(sim.ssg_inverted());
    ssg_fold_samples(sim, 1, 200000);
    sim.step();
    CHECK(!sim.ssg_inverted());
    ssg_fold_samples(sim, 1, 200000);
    sim.step();
    CHECK(sim.ssg_inverted());
    CHECK(!sim.is_static());
  }
  // Mode 7: one rise, then jump to silence.
  {
    EgSimulator sim = run(7, one_ramp * 2);
    CHECK_EQ(sim.attenuation(), 0x3FF);
    CHECK(!sim.ssg_inverted());
    CHECK_EQ(sim.output(), 0x3FF);
    CHECK(sim.is_static());
  }
}

void test_output_inversion_math() {
  // out = (0x200 - A) & 0x3FF when J = 1, then TL, then clamp.
  OperatorParams op = ssg_patch(15, 0, 15, 0x0C); // mode 4, always inverted
  EgSimulator sim(op, kRks0);
  sim.key_on();
  CHECK(sim.ssg_inverted());
  sim.step(); // the key state reaches the envelope one sample later
  for (int i = 0; i < 4000; ++i) {
    const uint16_t a = sim.attenuation();
    CHECK_EQ(sim.output(), static_cast<uint16_t>((0x200 - a) & 0x3FF));
    if (a >= kSsgFoldAttenuation)
      break;
    sim.step(30);
  }
  // TL slides the whole inverted shape down, it does not move the pivot.
  op.tl = 32;
  EgSimulator tl(op, kRks0);
  tl.key_on();
  tl.step(3000);
  const uint16_t a = tl.attenuation();
  CHECK_EQ(tl.output(), std::min<int>(((0x200 - a) & 0x3FF) + 32 * 8, 0x3FF));
}

// ---------------------------------------------------------------- section 3

void test_key_off_latches_inverted_level() {
  EgSimulator sim(ssg_patch(15, 0, 15, 0x0A), kRks0); // mode 2
  sim.key_on();
  uint64_t n = 0;
  while (!sim.ssg_inverted() && n < 200000) { // past the first fold
    sim.step();
    ++n;
  }
  CHECK(sim.ssg_inverted());
  while (sim.attenuation() < 128) {
    sim.step();
    ++n;
  }
  // Keep the key-off sample and the one after it clear of an EG tick, so no
  // release increment lands on top of the level the latch leaves.
  while (n % kEgClockDivider != 1) {
    sim.step();
    ++n;
  }
  const uint16_t internal = sim.attenuation();
  const uint16_t audible = sim.output();
  CHECK_EQ(audible, static_cast<uint16_t>((0x200 - internal) & 0x3FF));

  sim.key_off();
  CHECK(!sim.keyed_on());
  // The latch lands on the key-off sample; the flag is masked out on the next.
  sim.step();
  CHECK(sim.phase() == EgPhase::Release);
  // Release continues from the level that was audible, not the internal one.
  CHECK_EQ(sim.attenuation(), audible);
  CHECK_EQ(sim.output(), audible);
  sim.step();
  CHECK(!sim.ssg_inverted());
  CHECK_EQ(sim.attenuation(), audible);
}

void test_release_hard_cut_at_0x200() {
  EgSimulator sim(ssg_patch(15, 0, 15, 0x08, 31, /*rr=*/8), kRks0);
  sim.key_on();
  sim.step(2000); // partway down the ramp
  CHECK(sim.attenuation() < kSsgFoldAttenuation);
  sim.key_off();
  bool saw_cut = false;
  uint16_t prev = sim.attenuation();
  for (uint64_t i = 0; i < 400000; ++i) {
    sim.step();
    const uint16_t a = sim.attenuation();
    if (prev >= kSsgFoldAttenuation) {
      // One sample at/after 0x200, then hard cut to silence.
      CHECK_EQ(a, 0x3FF);
      saw_cut = true;
      break;
    }
    prev = a;
  }
  CHECK(saw_cut);
  CHECK(sim.is_static());
}

void test_release_is_also_four_times_faster() {
  auto release_ticks = [](int ssg) {
    OperatorParams op = ssg_patch(15, 0, 15, ssg, 31, /*rr=*/6);
    EgSimulator sim(op, kRks0);
    sim.key_on();
    CHECK_EQ(sim.attenuation(), 0);
    sim.key_off();
    return ticks_until(
        sim, [](EgSimulator &s) { return s.attenuation() >= 0x1F0; }, 200000);
  };
  const uint32_t plain = release_ticks(0x00);
  const uint32_t ssg = release_ticks(0x08);
  CHECK(plain > 0 && ssg > 0);
  CHECK_REL(static_cast<double>(plain) / static_cast<double>(ssg), 4.0, 0.02);
}

void test_key_off_is_edge_triggered() {
  EgSimulator sim(ssg_patch(15, 0, 15, 0x0A), kRks0);
  sim.key_on();
  ssg_fold_samples(sim, 1, 200000);
  sim.step();
  while (sim.attenuation() < 128)
    sim.step();
  sim.key_off();
  const uint16_t after = sim.attenuation();
  sim.key_off(); // second write with the same state does nothing
  CHECK_EQ(sim.attenuation(), after);
}

void test_sl15_leaves_dr_in_charge() {
  EgSimulator sim(ssg_patch(15, 31, 15, 0x08), kRks0);
  sim.key_on();
  bool entered_sustain = false;
  for (uint64_t i = 0; i < 20000; ++i) {
    sim.step();
    if (sim.phase() == EgPhase::Sustain)
      entered_sustain = true;
    if (sim.attenuation() >= kSsgFoldAttenuation)
      break;
  }
  CHECK(!entered_sustain); // 0x3E0 is unreachable under the 0x200 freeze
}

void test_sl_below_15_always_under_fold() {
  for (int sl = 0; sl <= 14; ++sl) {
    EgSimulator sim(ssg_patch(31, 15, sl, 0x08), kRks0);
    CHECK(sim.sustain_attenuation() < kSsgFoldAttenuation);
  }
  EgSimulator sim(ssg_patch(31, 15, 15, 0x08), kRks0);
  CHECK(sim.sustain_attenuation() > kSsgFoldAttenuation);
}

// With AR < 31 the SSG block runs on every sample of a whole attack, because
// the attack cannot pull the level below 0x200 in one go. The fold is the
// arrival, not each sample spent there, so the reported loop period is the
// envelope's cycle rather than one sample.
void test_fold_is_reported_once_per_cycle() {
  CurveRequest req;
  req.op = OperatorParams{14, 18, 14, 0, 9, 0, 0, 0x0C}; // AR=14, SSG type 4
  req.pitch = NotePitch::from_midi(60);
  req.gate_ms = -1.0;
  req.max_ms = 2000.0;
  const CurveResult r = sample_curve(req);

  size_t folds = 0;
  for (const Marker &m : r.markers)
    if (m.kind == MarkerKind::SsgFold)
      ++folds;
  CHECK(folds >= 2);
  // A per-sample fold would report the sample rate itself (~53 kHz).
  CHECK(r.loop_hz > 1.0);
  CHECK(r.loop_hz < 100.0);

  // Cross-check against the cycle an independent run measures.
  EgSimulator eg(req.op, req.pitch);
  eg.key_on();
  int crossings = 0;
  double first = -1.0, last = -1.0;
  bool below = true;
  for (int i = 0; i < 53267 * 2; ++i) {
    eg.step();
    const bool now = eg.attenuation() < 0x200;
    if (below && !now) {
      ++crossings;
      if (first < 0.0)
        first = eg.time_ms();
      else
        last = eg.time_ms();
    }
    below = now;
  }
  CHECK(crossings > 2);
  const double measured = (last - first) / (crossings - 1);
  CHECK_REL(1000.0 / r.loop_hz, measured, 0.15);
}

} // namespace

int main() {
  std::cout << "ssg_test\n";
  RUN_TEST(test_register_bit_decode);
  RUN_TEST(test_loop_period_vs_dr);
  RUN_TEST(test_loop_period_key_scaled);
  RUN_TEST(test_loop_period_vs_sr);
  RUN_TEST(test_sr0_never_loops);
  RUN_TEST(test_dr0_never_loops);
  RUN_TEST(test_one_sample_at_0x200_before_fold);
  RUN_TEST(test_four_times_increment_and_freeze);
  RUN_TEST(test_attack_phase_unaffected_by_ssg);
  RUN_TEST(test_mode_shapes);
  RUN_TEST(test_output_inversion_math);
  RUN_TEST(test_key_off_latches_inverted_level);
  RUN_TEST(test_release_hard_cut_at_0x200);
  RUN_TEST(test_release_is_also_four_times_faster);
  RUN_TEST(test_key_off_is_edge_triggered);
  RUN_TEST(test_sl15_leaves_dr_in_charge);
  RUN_TEST(test_sl_below_15_always_under_fold);
  RUN_TEST(test_fold_is_reported_once_per_cycle);
  return testing::summary();
}
