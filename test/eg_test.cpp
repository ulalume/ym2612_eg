// Base envelope generator: every numeric anchor in EG_SPEC.md.

#include "check.hpp"
#include "helpers.hpp"
#include "reference_eg.hpp"

#include "ym2612_eg/ym2612_eg.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace ym2612_eg;
using helpers::eg_tick;
using helpers::ticks_to_ms;
using helpers::ticks_until;

namespace {

// Middle C as used by EG_SPEC section 9.
constexpr NotePitch kC4{644, 4};
// Same F-num one and four octaves down / up.
constexpr NotePitch kC1{644, 1};
constexpr NotePitch kC7{644, 7};
// Block 0 keeps keycode < 8, i.e. ksv = 0 at KS = 0.
constexpr NotePitch kBlock0{644, 0};

// EG_SPEC section 9 patch: AR=31 TL=0 DR=10 SL=2 SR=5 RR=7 KS=0.
constexpr OperatorParams kWorked{31, 10, 5, 7, 2, 0, 0, 0};

// ---------------------------------------------------------------- section 4

void test_increment_table() {
  // Rows 0 and 1 are all-zero: rate 0 never advances.
  for (int i = 0; i < 8; ++i) {
    CHECK_EQ(detail::kIncTable[0][i], 0);
    CHECK_EQ(detail::kIncTable[1][i], 0);
  }
  const uint8_t p0101[8] = {0, 1, 0, 1, 0, 1, 0, 1};
  const uint8_t p0111[8] = {0, 1, 1, 1, 0, 1, 1, 1};
  const uint8_t p0101_1101[8] = {0, 1, 0, 1, 1, 1, 0, 1};
  const uint8_t p0111_1111[8] = {0, 1, 1, 1, 1, 1, 1, 1};

  for (int r = 2; r <= 5; ++r)
    for (int i = 0; i < 8; ++i)
      CHECK_EQ(detail::kIncTable[r][i], p0101[i]);
  for (int r = 6; r <= 7; ++r)
    for (int i = 0; i < 8; ++i)
      CHECK_EQ(detail::kIncTable[r][i], p0111[i]);

  // Rows 8..47 repeat a four-row group with period 4.
  for (int r = 8; r <= 47; ++r) {
    const uint8_t *want = (r % 4 == 0)   ? p0101
                          : (r % 4 == 1) ? p0101_1101
                          : (r % 4 == 2) ? p0111
                                         : p0111_1111;
    for (int i = 0; i < 8; ++i)
      CHECK_EQ(detail::kIncTable[r][i], want[i]);
  }

  const uint8_t high[16][8] = {
      {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2},
      {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},
      {2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4},
      {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},
      {4, 4, 4, 4, 4, 4, 4, 4}, {4, 4, 4, 8, 4, 4, 4, 8},
      {4, 8, 4, 8, 4, 8, 4, 8}, {4, 8, 8, 8, 4, 8, 8, 8},
      {8, 8, 8, 8, 8, 8, 8, 8}, {8, 8, 8, 8, 8, 8, 8, 8},
      {8, 8, 8, 8, 8, 8, 8, 8}, {8, 8, 8, 8, 8, 8, 8, 8}};
  for (int r = 48; r < 64; ++r)
    for (int i = 0; i < 8; ++i)
      CHECK_EQ(detail::kIncTable[r][i], high[r - 48][i]);
}

void test_rate_shift_table() {
  // EG_SPEC section 4 update-period table.
  const int want[12] = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  for (int rate = 0; rate < 64; ++rate) {
    const int group = rate / 4;
    const int expected = group < 11 ? want[group] : 0;
    CHECK_EQ(detail::rate_shift(rate), expected);
  }
  // rates 44..63 must all give shift 0 (do NOT derive it unclamped).
  for (int rate = 44; rate < 64; ++rate)
    CHECK_EQ(detail::rate_shift(rate), 0);
  // Period in EG ticks and the NTSC millisecond column.
  CHECK_REL(ticks_to_ms(1 << 11), 115.3, 0.005);
  CHECK_REL(ticks_to_ms(1 << 4), 0.901, 0.005);
  CHECK_REL(ticks_to_ms(1), 0.0563, 0.005);
}

// ---------------------------------------------------------------- section 1

void test_timing_constants() {
  CHECK_REL(kNtscClockHz, 7670453.57, 1e-6);
  CHECK_REL(kPalClockHz, 7600489.14, 1e-6);
  CHECK_REL(sample_rate_hz(kNtscClockHz), 53267.04, 1e-6);
  CHECK_REL(eg_rate_hz(kNtscClockHz), 17755.68, 1e-6);
  CHECK_REL(1e6 / eg_rate_hz(kNtscClockHz), 56.3200, 1e-5);
  CHECK_REL(1e6 / eg_rate_hz(kPalClockHz), 56.8384, 1e-5);
}

// The 12-bit counter skips 0 on overflow, so rates 0-3 fire once per 4095
// ticks rather than 4096, and counter index 0 is never selected.
void test_counter_skips_zero() {
  OperatorParams op{31, 1, 0, 15, 15, 0, 0, 0}; // DR=1 -> rate 2, shift 11
  EgSimulator sim(op, kBlock0);
  CHECK_EQ(sim.key_scale_value(), 0);
  CHECK_EQ(sim.rate_of(EgPhase::Decay), 2);
  sim.key_on();
  CHECK_EQ(sim.attenuation(), 0); // AR=31 at ksv 0 -> rate 62 -> instant

  const uint32_t first =
      ticks_until(sim, [](EgSimulator &s) { return s.attenuation() > 0; }, 9000);
  const uint32_t second = ticks_until(
      sim, [](EgSimulator &s) { return s.attenuation() > 1; }, 9000);
  CHECK_EQ(first, 2048u);  // counter reaches 2048
  CHECK_EQ(second, 4095u); // counter 1..4095 then wraps to 1, never to 0
}

// ---------------------------------------------------------------- section 5

void test_attack_formula_trajectory() {
  // att += (~att * inc) >> 4, arithmetic shift, starting from 0x3FF.
  auto run = [](int inc, int updates) {
    int att = 0x3FF;
    for (int i = 0; i < updates; ++i)
      att += (~att * inc) >> 4;
    return att;
  };
  CHECK_EQ(run(1, 1), 959);
  CHECK_EQ(run(1, 2), 899);
  CHECK_EQ(run(1, 4), 789);
  CHECK_EQ(run(1, 8), 607);
  CHECK_EQ(run(1, 16), 359);
  CHECK_EQ(run(1, 32), 121);
  CHECK_EQ(run(1, 64), 9);

  auto count = [](int inc) {
    int att = 0x3FF, n = 0;
    while (att != 0 && n < 1000) {
      att += (~att * inc) >> 4;
      ++n;
    }
    return n;
  };
  CHECK_EQ(count(1), 73);
  CHECK_EQ(count(2), 40);
  CHECK_EQ(count(4), 21);
  CHECK_EQ(count(8), 10);
}

// Rate 48 has increment 1 on every tick, so the simulator must walk exactly
// the trajectory above and reach 0 on update 73.
void test_attack_trajectory_in_simulator() {
  OperatorParams op{24, 10, 5, 7, 2, 0, 0, 0}; // AR=24, ksv=0 -> rate 48
  EgSimulator sim(op, kBlock0);
  CHECK_EQ(sim.rate_of(EgPhase::Attack), 48);
  sim.key_on();
  CHECK_EQ(sim.attenuation(), 0x3FF); // no instant attack below rate 62

  const int expect[7][2] = {{1, 959},  {2, 899},  {4, 789}, {8, 607},
                            {16, 359}, {32, 121}, {64, 9}};
  uint32_t done = 0;
  for (auto &e : expect) {
    while (done < static_cast<uint32_t>(e[0])) {
      eg_tick(sim);
      ++done;
    }
    CHECK_EQ(sim.attenuation(), e[1]);
  }
  while (done < 73) {
    eg_tick(sim);
    ++done;
  }
  CHECK_EQ(sim.attenuation(), 0);
  CHECK(sim.phase() == EgPhase::Attack); // transition is checked next tick
  eg_tick(sim);
  CHECK(sim.phase() == EgPhase::Decay);
}

// ---------------------------------------------------------------- section 3

void test_keycode_and_key_scaling() {
  CHECK_EQ(kC4.keycode(), 16);
  CHECK_EQ(kC1.keycode(), 4);
  CHECK_EQ(kC7.keycode(), 28);
  CHECK_EQ(kBlock0.keycode(), 0);

  // lsb = (F11 & (F10|F9|F8)) | (~F11 & F10 & F9 & F8)
  auto lsb_of = [](uint16_t fnum) {
    const int f11 = (fnum >> 10) & 1;
    const int f10 = (fnum >> 9) & 1;
    const int f9 = (fnum >> 8) & 1;
    const int f8 = (fnum >> 7) & 1;
    return (f11 & (f10 | f9 | f8)) | ((~f11 & 1) & f10 & f9 & f8);
  };
  for (uint16_t fnum = 0; fnum < 2048; ++fnum) {
    const NotePitch p{fnum, 3};
    const int want = (3 << 2) | (((fnum >> 10) & 1) << 1) | lsb_of(fnum);
    CHECK_EQ(p.keycode(), want);
  }

  // EG_SPEC section 9: the same patch at four KS settings.
  const int ksv[4] = {2, 4, 8, 16};
  const int rates[4][4] = {{63, 22, 12, 32},
                           {63, 24, 14, 34},
                           {63, 28, 18, 38},
                           {63, 36, 26, 46}};
  for (int ks = 0; ks < 4; ++ks) {
    OperatorParams op = kWorked;
    op.ks = static_cast<uint8_t>(ks);
    EgSimulator sim(op, kC4);
    CHECK_EQ(sim.key_scale_value(), ksv[ks]);
    CHECK_EQ(sim.rate_of(EgPhase::Attack), rates[ks][0]);
    CHECK_EQ(sim.rate_of(EgPhase::Decay), rates[ks][1]);
    CHECK_EQ(sim.rate_of(EgPhase::Sustain), rates[ks][2]);
    CHECK_EQ(sim.rate_of(EgPhase::Release), rates[ks][3]);
    CHECK_EQ(sim.sustain_attenuation(), 64);
  }

  // R = 0 ignores ksv entirely.
  OperatorParams zero{0, 0, 0, 0, 0, 0, 3, 0};
  EgSimulator z(zero, kC7);
  CHECK_EQ(z.rate_of(EgPhase::Attack), 0);
  CHECK_EQ(z.rate_of(EgPhase::Decay), 0);
  CHECK_EQ(z.rate_of(EgPhase::Sustain), 0);
  CHECK_EQ(z.rate_of(EgPhase::Release), 30); // RR=0 -> R=1 -> 2 + ksv(28)
}

// ---------------------------------------------------------------- section 9

void test_worked_example() {
  EgSimulator sim(kWorked, kC4);
  sim.key_on();
  // Instant attack: att forced to 0 at key-on, Decay entered on tick 1.
  CHECK_EQ(sim.attenuation(), 0);
  CHECK(sim.phase() == EgPhase::Attack);
  eg_tick(sim);
  CHECK(sim.phase() == EgPhase::Decay);

  // Decay 0 -> 64 (sustain level), tick 5440 = 306 ms.
  EgSimulator s2(kWorked, kC4);
  s2.key_on();
  const uint32_t d_end = ticks_until(
      s2, [](EgSimulator &s) { return s.phase() == EgPhase::Sustain; }, 20000);
  CHECK_EQ(d_end, 5440u);
  CHECK_EQ(s2.attenuation(), 64);
  CHECK_REL(ticks_to_ms(d_end), 306.4, 0.01);

  // Sustain 64 -> 0x3F0, tick 488585 ~ 27.5 s from key-on.
  const uint32_t quiet = ticks_until(
      s2, [](EgSimulator &s) { return s.attenuation() >= 0x3F0; }, 600000);
  CHECK_EQ(quiet + d_end, 488585u);
  CHECK_REL(ticks_to_ms(488585) / 1000.0, 27.5, 0.01);
  // Nuked behavior: reaching 0x3F0 snaps straight to 0x3FF and to Release.
  CHECK_EQ(s2.attenuation(), 0x3FF);
  CHECK(s2.phase() == EgPhase::Release);
}

void test_ks_sweep_durations() {
  // EG_SPEC section 9 "Same patch, KS swept".
  const uint32_t decay_ticks[4] = {5440, 4065, 2033, 509};
  const uint32_t quiet_ticks[4] = {488585, 326065, 163033, 40759};
  const double decay_ms[4] = {306.4, 228.9, 114.5, 28.7};
  const double sustain_s[4] = {27.5, 18.4, 9.2, 2.3};

  for (int ks = 0; ks < 4; ++ks) {
    OperatorParams op = kWorked;
    op.ks = static_cast<uint8_t>(ks);
    EgSimulator sim(op, kC4);
    sim.key_on();
    const uint32_t d = ticks_until(
        sim, [](EgSimulator &s) { return s.phase() == EgPhase::Sustain; },
        30000);
    CHECK_EQ(d, decay_ticks[ks]);
    CHECK_REL(ticks_to_ms(d), decay_ms[ks], 0.01);
    const uint32_t q = ticks_until(
        sim, [](EgSimulator &s) { return s.attenuation() >= 0x3F0; }, 600000);
    CHECK_EQ(q + d, quiet_ticks[ks]);
    CHECK_REL(ticks_to_ms(quiet_ticks[ks]) / 1000.0, sustain_s[ks], 0.01);
  }
}

void test_three_octaves() {
  // Identical registers, three octaves: AR=25, KS=3.
  struct Case {
    NotePitch pitch;
    int keycode, ksv;
    int rates[4];
    double attack_ms, decay_ms;
  };
  const Case cases[3] = {
      {kC1, 4, 4, {54, 24, 14, 34}, 1.58, 228.9},
      {kC4, 16, 16, {63, 36, 26, 46}, 0.06, 28.7},
      {kC7, 28, 28, {63, 48, 38, 58}, 0.06, 3.7},
  };
  for (const Case &c : cases) {
    OperatorParams op{25, 10, 5, 7, 2, 0, 3, 0};
    EgSimulator sim(op, c.pitch);
    CHECK_EQ(c.pitch.keycode(), c.keycode);
    CHECK_EQ(sim.key_scale_value(), c.ksv);
    CHECK_EQ(sim.rate_of(EgPhase::Attack), c.rates[0]);
    CHECK_EQ(sim.rate_of(EgPhase::Decay), c.rates[1]);
    CHECK_EQ(sim.rate_of(EgPhase::Sustain), c.rates[2]);
    CHECK_EQ(sim.rate_of(EgPhase::Release), c.rates[3]);

    sim.key_on();
    const uint32_t a = ticks_until(
        sim, [](EgSimulator &s) { return s.phase() != EgPhase::Attack; }, 5000);
    CHECK_ABS(ticks_to_ms(a), c.attack_ms, std::max(0.01, c.attack_ms * 0.01));
    const uint32_t d = ticks_until(
        sim, [](EgSimulator &s) { return s.phase() == EgPhase::Sustain; },
        30000);
    // The spec prints these to 0.1 ms, so allow half a display digit.
    CHECK_ABS(ticks_to_ms(a + d), c.decay_ms,
              std::max(c.decay_ms * 0.01, 0.05));
  }
}

void test_attack_shape_anchors() {
  // EG_SPEC section 9: DR/SL as the worked example, KS=0, C4 (ksv 2).
  struct Case {
    int ar, rate;
    uint32_t ticks;
    double ms;
  };
  const Case cases[6] = {{12, 26, 3105, 174.87}, {16, 34, 777, 43.76},
                         {20, 42, 195, 10.98},   {24, 50, 52, 2.93},
                         {28, 58, 14, 0.79},     {31, 63, 1, 0.06}};
  for (const Case &c : cases) {
    OperatorParams op = kWorked;
    op.ar = static_cast<uint8_t>(c.ar);
    EgSimulator sim(op, kC4);
    CHECK_EQ(sim.rate_of(EgPhase::Attack), c.rate);
    sim.key_on();
    const uint32_t t = ticks_until(
        sim, [](EgSimulator &s) { return s.phase() != EgPhase::Attack; },
        20000);
    CHECK_EQ(t, c.ticks);
    CHECK_ABS(ticks_to_ms(t), c.ms, std::max(0.01, c.ms * 0.01));
  }
}

// ---------------------------------------------------------------- section 6

void test_release_worked_example() {
  EgSimulator sim(kWorked, kC4);
  sim.key_on();
  for (uint32_t t = 0; t < 5999; ++t) // key-off lands at the top of tick 6000
    eg_tick(sim);
  CHECK_EQ(sim.attenuation(), 65);
  sim.key_off();
  CHECK(sim.phase() == EgPhase::Release);
  CHECK(!sim.keyed_on());

  const uint32_t quiet = ticks_until(
      sim, [](EgSimulator &s) { return s.attenuation() >= 0x3F0; }, 60000);
  CHECK_EQ(quiet, 15076u);
  CHECK_REL(ticks_to_ms(quiet), 849.0, 0.01);
  // Nuked behavior: 0x3F0 and 0x3FF now coincide.
  CHECK_EQ(sim.attenuation(), 0x3FF);

  // Without the snap (ymfm / jsgroth path) the tail creeps on to 862.5 ms.
  refeg::Reference ref;
  ref.ar = 31;
  ref.dr = 10;
  ref.sr = 5;
  ref.rr = 7;
  ref.sl = 2;
  ref.keycode = kC4.keycode();
  ref.snap = false;
  ref.configure();
  ref.key_on();
  while (ref.ticks < 5999)
    ref.tick();
  CHECK_EQ(ref.att, 65);
  ref.key_off();
  uint32_t at_3f0 = 0, at_3ff = 0;
  while (ref.ticks < 60000 && at_3ff == 0) {
    ref.tick();
    if (at_3f0 == 0 && ref.att >= 0x3F0)
      at_3f0 = ref.ticks;
    if (ref.att == 0x3FF)
      at_3ff = ref.ticks;
  }
  CHECK_EQ(at_3f0, 21075u);
  CHECK_EQ(at_3ff, 21315u);
  CHECK_REL(ticks_to_ms(at_3f0 - 6000), 849.0, 0.01);
  CHECK_REL(ticks_to_ms(at_3ff - 6000), 862.5, 0.01);
}

void test_sustain_level_mapping() {
  for (int sl = 0; sl < 16; ++sl) {
    OperatorParams op = kWorked;
    op.sl = static_cast<uint8_t>(sl);
    EgSimulator sim(op, kC4);
    const int want = sl == 15 ? 0x3E0 : sl * 32;
    CHECK_EQ(sim.sustain_attenuation(), want);
  }
}

// SL=0 must skip decay entirely, with zero decay updates.
void test_sl0_skips_decay() {
  OperatorParams op{31, 10, 5, 7, 0, 0, 0, 0};
  EgSimulator sim(op, kC4);
  sim.key_on();
  CHECK_EQ(sim.attenuation(), 0);
  eg_tick(sim); // Attack -> Decay -> Sustain, both on the same tick
  CHECK(sim.phase() == EgPhase::Sustain);
  CHECK_EQ(sim.attenuation(), 0);
}

void test_sr0_holds_forever() {
  OperatorParams op{31, 10, 0, 7, 2, 0, 0, 0}; // SR = 0
  EgSimulator sim(op, kC4);
  sim.key_on();
  const uint32_t d = ticks_until(
      sim, [](EgSimulator &s) { return s.phase() == EgPhase::Sustain; }, 20000);
  CHECK_EQ(d, 5440u);
  CHECK_EQ(sim.rate_of(EgPhase::Sustain), 0);
  CHECK(sim.is_static());
  const uint16_t held = sim.attenuation();
  sim.step(600000);
  CHECK_EQ(sim.attenuation(), held);
  CHECK(sim.is_static());
}

void test_dr0_holds() {
  OperatorParams op{31, 0, 5, 7, 2, 0, 0, 0}; // DR = 0, SL > 0
  EgSimulator sim(op, kC4);
  sim.key_on();
  CHECK_EQ(sim.rate_of(EgPhase::Decay), 0);
  sim.step(300000);
  CHECK(sim.phase() == EgPhase::Decay);
  CHECK_EQ(sim.attenuation(), 0);
  CHECK(sim.is_static());
}

// ---------------------------------------------------------------- section 7

void test_attack_freeze_rates_62_63() {
  // Raising the attack rate to >= 62 mid-attack freezes attenuation forever.
  OperatorParams op{16, 10, 5, 7, 2, 0, 0, 0}; // rate 34 at C4
  EgSimulator sim(op, kC4);
  sim.key_on();
  sim.step(3 * 200);
  const uint16_t frozen = sim.attenuation();
  CHECK(frozen > 0 && frozen < 0x3FF);
  CHECK(!sim.is_static());

  OperatorParams fast = op;
  fast.ar = 31; // rate 63 -> `rate < 62` guard blocks every update
  sim.set_params(fast);
  CHECK_EQ(sim.rate_of(EgPhase::Attack), 63);
  CHECK(sim.is_static());
  sim.step(3 * 100000);
  CHECK_EQ(sim.attenuation(), frozen);
  CHECK(sim.phase() == EgPhase::Attack);
}

void test_attack_freeze_rate_zero() {
  OperatorParams op{0, 10, 5, 7, 2, 0, 0, 0}; // AR = 0 -> rate 0
  EgSimulator sim(op, kC4);
  sim.key_on();
  CHECK_EQ(sim.rate_of(EgPhase::Attack), 0);
  CHECK_EQ(sim.attenuation(), 0x3FF);
  sim.step(); // the key state reaches the envelope one sample later
  CHECK(sim.is_static());
  sim.step(3 * 100000);
  CHECK_EQ(sim.attenuation(), 0x3FF);
  CHECK(sim.phase() == EgPhase::Attack);
}

void test_key_edges_and_retrigger() {
  EgSimulator sim(kWorked, kC4);
  CHECK(!sim.keyed_on());
  sim.key_off(); // no-op while already off
  CHECK(sim.phase() == EgPhase::Release);
  CHECK_EQ(sim.attenuation(), 0x3FF);

  sim.key_on();
  CHECK(sim.keyed_on());
  CHECK_EQ(sim.attenuation(), 0);
  ticks_until(sim, [](EgSimulator &s) { return s.attenuation() >= 300; },
              20000);
  const uint16_t mid = sim.attenuation();

  // Double key-on is a no-op: no instant-attack reset, no phase change.
  sim.key_on();
  CHECK_EQ(sim.attenuation(), mid);
  CHECK(sim.phase() == EgPhase::Decay || sim.phase() == EgPhase::Sustain);

  // A real retrigger restarts the attack from the current attenuation.
  OperatorParams slow = kWorked;
  slow.ar = 16; // rate 34, not instant
  EgSimulator s2(slow, kC4);
  s2.key_on();
  ticks_until(s2, [](EgSimulator &s) { return s.attenuation() <= 500; }, 20000);
  s2.key_off();
  ticks_until(s2, [](EgSimulator &s) { return s.attenuation() >= 700; }, 20000);
  const uint16_t before = s2.attenuation();
  s2.key_on();
  CHECK_EQ(s2.attenuation(), before); // attenuation untouched by key-on
  CHECK(s2.phase() == EgPhase::Attack);
  s2.step(3 * 16); // rate 34 updates once every 8 ticks
  CHECK(s2.attenuation() < before);
}

void test_key_off_from_any_phase() {
  for (int phase_step : {0, 1, 2}) {
    OperatorParams op{16, 10, 5, 7, 2, 0, 0, 0};
    EgSimulator sim(op, kC4);
    sim.key_on();
    if (phase_step >= 1)
      ticks_until(sim, [](EgSimulator &s) { return s.phase() == EgPhase::Decay; },
                  20000);
    if (phase_step >= 2)
      ticks_until(sim,
                  [](EgSimulator &s) { return s.phase() == EgPhase::Sustain; },
                  20000);
    const uint16_t att = sim.attenuation();
    sim.key_off();
    CHECK(sim.phase() == EgPhase::Release);
    CHECK_EQ(sim.attenuation(), att); // untouched on the non-SSG path
  }
}

void test_counter_phase_reset() {
  // reset(counter_phase) presets the free-running counter; a different phase
  // shifts the first update by up to one period.
  OperatorParams op{31, 10, 5, 7, 15, 0, 0, 0};
  EgSimulator a(op, kC4), b(op, kC4);
  a.reset(0);
  b.reset(2000);
  a.key_on();
  b.key_on();
  const uint32_t ta =
      ticks_until(a, [](EgSimulator &s) { return s.attenuation() > 0; }, 5000);
  const uint32_t tb =
      ticks_until(b, [](EgSimulator &s) { return s.attenuation() > 0; }, 5000);
  CHECK(ta > 0 && tb > 0);
  CHECK(ta != tb);
  // start_att presets attenuation for retrigger visualisation.
  EgSimulator c(op, kC4);
  c.reset(0, 512);
  CHECK_EQ(c.attenuation(), 512);
}

// A skip is only ever allowed to stand in for the same number of step()
// calls: nothing observable may move across the samples it names, and the
// state it lands on -- including the counter, the divider and the SSG latches
// that only show up later -- must be the state stepping would have reached.
void test_skip_matches_stepping() {
  struct Obs {
    uint16_t att, out;
    uint8_t phase;
    bool on, inverted, at_rest;
    bool operator==(const Obs &o) const {
      return att == o.att && out == o.out && phase == o.phase && on == o.on &&
             inverted == o.inverted && at_rest == o.at_rest;
    }
  };
  const auto look = [](const EgSimulator &s) {
    return Obs{s.attenuation(), s.output(), static_cast<uint8_t>(s.phase()),
               s.keyed_on(), s.ssg_inverted(), s.is_static()};
  };

  const uint8_t kR[6] = {0, 1, 6, 13, 24, 31};
  const uint16_t kStart[4] = {0, 0x1FF, 0x200, 0x3FF};
  const uint64_t kSpan = 20000;
  size_t claims = 0;
  size_t skipped = 0;
  bool ok = true;
  for (int ssg = 0; ssg < 16 && ok; ++ssg)
    for (size_t r = 0; r < 6 && ok; ++r) {
      OperatorParams op;
      op.ar = kR[r];
      op.dr = kR[(r + 2) % 6];
      op.sr = kR[(r + 4) % 6];
      op.rr = static_cast<uint8_t>(kR[(r + 1) % 6] / 2);
      op.sl = static_cast<uint8_t>((r * 5) % 16);
      op.tl = static_cast<uint8_t>((r * 23) % 128);
      op.ks = static_cast<uint8_t>(r % 4);
      op.ssg = static_cast<uint8_t>(ssg);
      const uint64_t gate = (r % 3 == 0) ? kSpan : 7000;
      EgSimulator sim(op, kC4);
      sim.reset(0, kStart[r % 4]);
      sim.key_on();
      for (uint64_t i = 0; i < kSpan && ok;) {
        if (i == gate)
          sim.key_off();
        uint64_t room = kSpan - i;
        if (i < gate && gate - i < room)
          room = gate - i;
        const uint64_t n = std::min<uint64_t>(sim.skippable_samples(), room);
        if (n == 0) {
          sim.step();
          ++i;
          continue;
        }
        EgSimulator stepped = sim, jumped = sim;
        const Obs before = look(sim);
        for (uint64_t k = 0; k < n && ok; ++k) {
          stepped.step();
          ok = look(stepped) == before;
        }
        jumped.skip(static_cast<uint32_t>(n));
        ok = ok && look(jumped) == look(stepped) &&
             jumped.time_ms() == stepped.time_ms();
        // The counter and the divider are invisible until they gate the next
        // update, so the two runs have to stay together afterwards too.
        if (ok && (claims % 16) == 0)
          for (int k = 0; k < 200 && ok; ++k) {
            stepped.step();
            jumped.step();
            ok = look(jumped) == look(stepped);
          }
        ++claims;
        skipped += static_cast<size_t>(n);
        sim.skip(static_cast<uint32_t>(n));
        i += n;
      }
    }
  CHECK(ok);
  // The sweep has to actually exercise the thing.
  CHECK(claims > 1000);
  CHECK(skipped > 100000);
}

// The two levels an alternate fold reports have to be the levels the next
// samples really carry, they have to hold for the whole run it names, and
// crossing the run has to leave the inversion flag on the parity a flip per
// sample would have left it on.
void test_alternating_run_matches_stepping() {
  const uint16_t kStart[5] = {0x100, 0x1FF, 0x200, 0x2A0, 0x3FF};
  size_t claims = 0, crossed = 0;
  bool ok = true;
  for (int ssg = 8; ssg < 16 && ok; ++ssg)
    for (int ar = 0; ar < 32 && ok; ++ar)
      for (int ks = 0; ks < 4 && ok; ++ks) {
        OperatorParams op;
        op.ar = static_cast<uint8_t>(ar);
        op.dr = static_cast<uint8_t>((ar * 7) % 32);
        op.sr = static_cast<uint8_t>((ar * 5) % 32);
        op.rr = static_cast<uint8_t>(ar % 16);
        op.sl = static_cast<uint8_t>((ar * 3) % 16);
        op.tl = static_cast<uint8_t>((ar * 11) % 128);
        op.ks = static_cast<uint8_t>(ks);
        op.ssg = static_cast<uint8_t>(ssg);
        const uint64_t kSpan = 30000;
        const uint64_t gate = (ar % 3 == 0) ? kSpan : 11000;
        EgSimulator sim(op, kC4);
        sim.reset(0, kStart[static_cast<size_t>(ar) % 5]);
        sim.key_on();
        for (uint64_t i = 0; i < kSpan && ok;) {
          if (i == gate)
            sim.key_off();
          uint64_t room = kSpan - i;
          if (i < gate && gate - i < room)
            room = gate - i;
          uint16_t first = 0, second = 0;
          const uint64_t n =
              std::min<uint64_t>(sim.alternating_samples(first, second), room);
          if (n == 0) {
            sim.step();
            ++i;
            continue;
          }
          EgSimulator stepped = sim, jumped = sim;
          const uint16_t att0 = sim.attenuation();
          const EgPhase ph0 = sim.phase();
          for (uint64_t k = 0; k < n && ok; ++k) {
            stepped.step();
            ok = stepped.output() == ((k & 1) ? second : first) &&
                 stepped.attenuation() == att0 && stepped.phase() == ph0;
          }
          jumped.skip(static_cast<uint32_t>(n));
          ok = ok && jumped.output() == stepped.output() &&
               jumped.attenuation() == stepped.attenuation() &&
               jumped.ssg_inverted() == stepped.ssg_inverted() &&
               jumped.phase() == stepped.phase() &&
               jumped.time_ms() == stepped.time_ms();
          for (int k = 0; k < 200 && ok; ++k) {
            stepped.step();
            jumped.step();
            ok = jumped.output() == stepped.output() &&
                 jumped.attenuation() == stepped.attenuation() &&
                 jumped.ssg_inverted() == stepped.ssg_inverted();
          }
          ++claims;
          crossed += static_cast<size_t>(n);
          sim.skip(static_cast<uint32_t>(n));
          i += n;
        }
      }
  CHECK(ok);
  // Only the alternate modes without hold reach it, so the sweep has to find
  // it there and nowhere else.
  CHECK(claims > 200);
  CHECK(crossed > 20000);
}

// ---------------------------------------------------------------- section 2

void test_tl_and_units() {
  OperatorParams op = kWorked;
  op.tl = 64;
  EgSimulator sim(op, kC4);
  sim.key_on();
  CHECK_EQ(sim.attenuation(), 0);
  CHECK_EQ(sim.output(), 64 * 8); // TL is added to the output, not to state
  op.tl = 127;
  sim.set_params(op);
  CHECK_EQ(sim.output(), 127 * 8);
  sim.reset(0, 0x3FF);
  CHECK_EQ(sim.output(), 0x3FF); // clamped

  CHECK_REL(atten_to_db(1023), 96.23552674, 1e-6);
  CHECK_REL(atten_to_db(1), 0.0940718736, 1e-6);
  CHECK_REL(atten_to_db(8), 0.7525749892, 1e-6);  // one TL step
  CHECK_REL(atten_to_db(32), 3.0102999566, 1e-6); // one SL step
  CHECK_REL(atten_to_amplitude(0), 1.0, 1e-12);
  CHECK_REL(atten_to_amplitude(64), 0.5, 1e-12);
  CHECK_REL(atten_to_amplitude(128), 0.25, 1e-12);
}

// megatoy src/ym2612/note.hpp: fnote_from_key() + Note::from_midi_note()
// + frequency_with_bend(note, 0).
void test_from_midi_matches_megatoy() {
  const uint16_t table[12] = {644, 682, 723, 766, 811, 859,
                              910, 965, 1022, 1083, 1147, 1215};
  // One full octave, MIDI 60..71 (megatoy octave 4).
  for (int m = 60; m <= 71; ++m) {
    const NotePitch p = NotePitch::from_midi(m);
    CHECK_EQ(p.fnum, table[m % 12]);
    CHECK_EQ(p.block, 4);
  }
  // Every note 12..127: block = clamp(midi/12 - 1, 0, 7).
  for (int m = 12; m <= 127; ++m) {
    const NotePitch p = NotePitch::from_midi(m);
    const int block = std::min(m / 12 - 1, 7);
    CHECK_EQ(p.fnum, table[m % 12]);
    CHECK_EQ(p.block, block);
  }
  // Extremes.  megatoy clamps negative octaves (MIDI 0..11) up to block 0,
  // the lowest representable octave, while keeping the pitch-class F-num.
  CHECK_EQ(NotePitch::from_midi(0).fnum, 644);
  CHECK_EQ(NotePitch::from_midi(0).block, 0);
  CHECK_EQ(NotePitch::from_midi(11).fnum, 1215);
  CHECK_EQ(NotePitch::from_midi(11).block, 0);
  CHECK_EQ(NotePitch::from_midi(12).block, 0);
  CHECK_EQ(NotePitch::from_midi(127).fnum, 965);
  CHECK_EQ(NotePitch::from_midi(127).block, 7);
  // Out-of-range input is clamped into the MIDI range.
  CHECK_EQ(NotePitch::from_midi(-5).fnum, NotePitch::from_midi(0).fnum);
  CHECK_EQ(NotePitch::from_midi(999).fnum, NotePitch::from_midi(127).fnum);

  // C4 (MIDI 60) is EG_SPEC's F-num 644 / block 4 example.
  CHECK_EQ(NotePitch::from_midi(60).keycode(), kC4.keycode());
}

// MIDI 60 is middle C and MIDI 69 is A440; C4..B4 get the key codes MDSDRV
// and Furnace give the same pitches.
void test_from_midi_is_standard_pitch() {
  const NotePitch c4 = NotePitch::from_midi(60);
  CHECK_EQ(c4.fnum, 644);
  CHECK_EQ(c4.block, 4);

  // Hz = fnum * 2^(block - 1) * clock / 144 / 2^20.
  const NotePitch a4 = NotePitch::from_midi(69);
  const double a4_hz = std::ldexp(static_cast<double>(a4.fnum), a4.block - 1) *
                       7670454.0 / 144.0 / 1048576.0;
  CHECK_REL(a4_hz, 440.0, 0.005);

  const int keycodes[12] = {16, 16, 16, 16, 16, 16, 17, 17, 17, 18, 18, 19};
  for (int m = 60; m <= 71; ++m) {
    CHECK_EQ(NotePitch::from_midi(m).keycode(), keycodes[m - 60]);
  }
}

// ------------------------------------------------------- cross-check vs ref

void test_cross_check_against_reference() {
  helpers::Rng rng(12345);
  int compared = 0;
  for (int trial = 0; trial < 120; ++trial) {
    OperatorParams op{};
    op.ar = static_cast<uint8_t>(rng.in(0, 31));
    op.dr = static_cast<uint8_t>(rng.in(0, 31));
    op.sr = static_cast<uint8_t>(rng.in(0, 31));
    op.rr = static_cast<uint8_t>(rng.in(0, 15));
    op.sl = static_cast<uint8_t>(rng.in(0, 15));
    op.tl = static_cast<uint8_t>(rng.in(0, 127));
    op.ks = static_cast<uint8_t>(rng.in(0, 3));
    const NotePitch pitch{static_cast<uint16_t>(rng.in(0, 2047)),
                          static_cast<uint8_t>(rng.in(0, 7))};
    const uint32_t key_off_tick = static_cast<uint32_t>(rng.in(1, 6000));

    EgSimulator sim(op, pitch);
    refeg::Reference ref;
    ref.ar = op.ar;
    ref.dr = op.dr;
    ref.sr = op.sr;
    ref.rr = op.rr;
    ref.sl = op.sl;
    ref.ks = op.ks;
    ref.tl = op.tl;
    ref.keycode = pitch.keycode();
    ref.snap = true;
    ref.configure();

    sim.key_on();
    ref.key_on();
    bool ok = true;
    for (uint32_t t = 1; t <= 8000 && ok; ++t) {
      if (t == key_off_tick) {
        sim.key_off();
        ref.key_off();
      }
      eg_tick(sim);
      ref.tick();
      if (sim.attenuation() != ref.att ||
          sim.output() != static_cast<uint16_t>(ref.output()) ||
          static_cast<int>(sim.phase()) != ref.state) {
        ok = false;
        std::cerr << "\n    trial " << trial << " tick " << t << ": att "
                  << sim.attenuation() << " vs " << ref.att << ", phase "
                  << static_cast<int>(sim.phase()) << " vs " << ref.state;
      }
    }
    CHECK(ok);
    ++compared;
  }
  CHECK_EQ(compared, 120);
}

} // namespace

int main() {
  std::cout << "eg_test\n";
  RUN_TEST(test_increment_table);
  RUN_TEST(test_rate_shift_table);
  RUN_TEST(test_timing_constants);
  RUN_TEST(test_counter_skips_zero);
  RUN_TEST(test_attack_formula_trajectory);
  RUN_TEST(test_attack_trajectory_in_simulator);
  RUN_TEST(test_keycode_and_key_scaling);
  RUN_TEST(test_worked_example);
  RUN_TEST(test_ks_sweep_durations);
  RUN_TEST(test_three_octaves);
  RUN_TEST(test_attack_shape_anchors);
  RUN_TEST(test_release_worked_example);
  RUN_TEST(test_sustain_level_mapping);
  RUN_TEST(test_sl0_skips_decay);
  RUN_TEST(test_sr0_holds_forever);
  RUN_TEST(test_dr0_holds);
  RUN_TEST(test_attack_freeze_rates_62_63);
  RUN_TEST(test_attack_freeze_rate_zero);
  RUN_TEST(test_key_edges_and_retrigger);
  RUN_TEST(test_key_off_from_any_phase);
  RUN_TEST(test_counter_phase_reset);
  RUN_TEST(test_skip_matches_stepping);
  RUN_TEST(test_alternating_run_matches_stepping);
  RUN_TEST(test_tl_and_units);
  RUN_TEST(test_from_midi_matches_megatoy);
  RUN_TEST(test_from_midi_is_standard_pitch);
  RUN_TEST(test_cross_check_against_reference);
  return testing::summary();
}
