// Golden-vector generator: drives Nuked-OPN2 at the register level and writes
// golden/*.json for test/golden_test.cpp to replay against EgSimulator.
//
// LICENSING: Nuked-OPN2 is LGPL 2.1 and ym2612_eg is MIT.  Nuked is fetched and
// linked *only* by this tool, which is built solely when
// -DYM2612_EG_BUILD_GOLDEN_GEN=ON.  Neither the library nor the default test
// suite ever sees it -- the tests consume the committed JSON, which is data.
//
// Usage: ym2612_eg_golden_gen <output-directory>

#include <ym2612_eg/ym2612_eg.hpp>

#include "golden_common.hpp"

extern "C" {
#include "ym3438.h"
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ym2612_eg;
using namespace ym2612_eg::golden;

// ---------------------------------------------------------------------------
// Chip driver
// ---------------------------------------------------------------------------

namespace {

// Channel 0 operator 1.  OPN2_KeyOn maps OP1 of channel c to slot c
// (mode_kon[chan]), and OPN2_DoRegWrite matches slot registers against
// op_offset[cycles % 12], where op_offset[0] == 0x000 -- so the $30/$40/... low
// nibble 0 addresses this slot.  $28 selects it with channel 0 and bit 4.
constexpr int kSlot = 0;
constexpr Bit8u kKeyOnBits = 0x10; // OP1 only, channel 0

// One output sample is 24 internal cycles (24 operator slots).
constexpr int kCyclesPerSample = 24;

struct Chip {
  ym3438_t st{};
  Bit16s discard[2]{};

  void clock(int n) {
    for (int i = 0; i < n; ++i)
      OPN2_Clock(&st, discard);
  }

  // Nuked latches a write over the following cycles (OPN2_DoIO shifts write_a /
  // write_d, then OPN2_DoRegWrite applies it on the cycle that matches the
  // slot).  Clocking a full sample after each half of the write is far more
  // than enough and keeps setup deterministic.
  void write_reg(Bit8u address, Bit8u data) {
    OPN2_Write(&st, 0, address);
    clock(kCyclesPerSample);
    OPN2_Write(&st, 1, data);
    clock(2 * kCyclesPerSample);
  }

  // $28 written inside one sample: the address goes out, two cycles later the
  // data, and the rest of the sample finishes.  The key transition then becomes
  // visible in eg_kon[] exactly two samples later (mode_kon is sampled by
  // OPN2_KeyOn at cycle 0, and eg_kon_latch one sample after that).
  void write_gate_within_sample(bool on) {
    OPN2_Write(&st, 0, 0x28);
    clock(2);
    OPN2_Write(&st, 1, on ? kKeyOnBits : 0x00);
    clock(kCyclesPerSample - 2);
  }
};

constexpr int kGateLatency = 2; // samples between the $28 write and eg_kon

} // namespace

// ---------------------------------------------------------------------------
// Scenario description
// ---------------------------------------------------------------------------

namespace {

struct Gate {
  int sample; // desired *effective* sample (when eg_kon changes)
  bool on;
};

struct Case {
  std::string name;
  OperatorParams op;
  NotePitch pitch;
  int samples;
  std::vector<Gate> gate;
};

struct Scenario {
  std::string file;
  std::string title;
  std::string description;
  std::vector<Case> cases;
};

// Nuked's state machine needs one spare output sample after a key transition
// before the next EG tick, otherwise the tick is spent on a state transition
// instead of an increment.  EG ticks sit on samples that are multiples of 3, so
// an effective key event must land on sample % 3 == 1.
int align_gate(int sample) {
  while (sample % 3 != 1)
    ++sample;
  return sample;
}

struct Trace {
  int counter_phase = 0;
  std::vector<uint16_t> level;
  std::vector<uint8_t> state;
  std::vector<uint16_t> out;
  std::vector<Gate> gate; // effective samples
};

Trace record(const Case &c) {
  Chip chip;
  OPN2_SetChipType(ym3438_mode_ym2612);
  OPN2_Reset(&chip.st);
  chip.clock(4 * kCyclesPerSample);

  chip.write_reg(0x30, 0x01);                                   // DT/MUL
  chip.write_reg(0x40, c.op.tl & 0x7F);                         // TL
  chip.write_reg(0x50, static_cast<Bit8u>(((c.op.ks & 3) << 6) |
                                          (c.op.ar & 0x1F)));   // KS/AR
  chip.write_reg(0x60, c.op.dr & 0x1F);                         // AM/DR
  chip.write_reg(0x70, c.op.sr & 0x1F);                         // SR
  chip.write_reg(0x80, static_cast<Bit8u>(((c.op.sl & 0x0F) << 4) |
                                          (c.op.rr & 0x0F)));   // SL/RR
  chip.write_reg(0x90, c.op.ssg & 0x0F);                        // SSG-EG
  chip.write_reg(0xA4, static_cast<Bit8u>(((c.pitch.block & 7) << 3) |
                                          ((c.pitch.fnum >> 8) & 7)));
  chip.write_reg(0xA0, static_cast<Bit8u>(c.pitch.fnum & 0xFF));
  chip.write_reg(0xB0, 0x00); // algorithm / feedback (irrelevant to the EG)
  chip.write_reg(0xB4, 0xC0); // both pans on

  // Land on a cycle-0 boundary so every subsequent block of 24 clocks is one
  // whole output sample.
  while (chip.st.cycles != 0)
    chip.clock(1);

  // Nuked bumps eg_timer once every three samples, on the sample where
  // eg_quotient reaches 2 -- that is an EG tick.  The increment used on a tick
  // comes from eg_shift_lock / eg_timer_low_lock, which are latched one tick
  // late, so the counter value that drives tick k is the eg_timer of tick k-1.
  // Collect two ticks, then start the vector on the next one.
  std::vector<int> tick_timer;
  while (tick_timer.size() < 2) {
    chip.clock(kCyclesPerSample);
    if (chip.st.eg_quotient == 2)
      tick_timer.push_back(chip.st.eg_timer);
  }
  while (true) {
    chip.clock(kCyclesPerSample);
    if (chip.st.eg_quotient == 2)
      break;
  }

  Trace t;
  // Sample 0 of the vector is an EG tick, matching EgSimulator's divider phase
  // after reset().  Its counter value must be tick_timer.back(), so the phase
  // handed to reset() is one step before that.
  t.counter_phase = counter_unstep(tick_timer.back());
  t.level.resize(c.samples);
  t.state.resize(c.samples);
  t.out.resize(c.samples);
  t.level[0] = chip.st.eg_level[kSlot];
  t.state[0] = chip.st.eg_state[kSlot];
  t.out[0] = chip.st.eg_out[kSlot];

  size_t next_gate = 0;
  bool keyed = false;
  for (int i = 1; i < c.samples; ++i) {
    if (next_gate < c.gate.size() &&
        i == c.gate[next_gate].sample - kGateLatency) {
      chip.write_gate_within_sample(c.gate[next_gate].on);
      ++next_gate;
    } else {
      chip.clock(kCyclesPerSample);
    }
    t.level[i] = chip.st.eg_level[kSlot];
    t.state[i] = chip.st.eg_state[kSlot];
    t.out[i] = chip.st.eg_out[kSlot];
    const bool now = chip.st.eg_kon[kSlot] != 0;
    if (now != keyed) {
      t.gate.push_back({i, now});
      keyed = now;
    }
  }
  return t;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

struct Verdict {
  bool ok = true;
  std::string why;
  int shift = 0;
};

Verdict verify(const Case &c, const Trace &t, bool *out_differs) {
  Verdict v;
  EgSimulator eg(c.op, c.pitch);

  bool mixed = false;
  int shift = counter_shift_for_case(eg, &mixed);
  if (mixed) {
    v.ok = false;
    v.why = "phases demand different eg_timer_low_lock shifts (mixed <48/>=48)";
    return v;
  }
  if (shift == kNoConstraint)
    shift = 0;
  v.shift = shift;

  if (has_sl0_instant_attack_divergence(eg)) {
    v.ok = false;
    v.why = "SL=0 with instant attack: Nuked spends the first EG tick on the "
            "second state transition (see golden/DISCREPANCIES.md)";
    return v;
  }
  if (has_ssg_sustain_window_divergence(c.op, eg)) {
    v.ok = false;
    v.why = "SSG-EG with a decay step > 15 can jump Nuked's 16-wide "
            "Decay->Sustain equality window (see golden/DISCREPANCIES.md)";
    return v;
  }
  if (shift != 0 && counter_wraps(t.counter_phase, c.samples)) {
    v.ok = false;
    v.why = "a counter-shifted case must stay inside one sweep of the 12-bit "
            "EG counter; shorten `samples`";
    return v;
  }

  eg.reset(static_cast<uint16_t>(apply_counter_shift(t.counter_phase, shift)));
  size_t g = 0;
  *out_differs = false;
  const bool check_out = output_comparable(c.op);
  for (int i = 0; i < c.samples; ++i) {
    while (g < t.gate.size() && t.gate[g].sample == i) {
      if (t.gate[g].on) {
        eg.key_on();
      } else {
        const char *bad = nullptr;
        if (has_ssg_alternate_keyon_parity_divergence(c.op, eg))
          bad = "SSG-EG alternate without hold under an attack below rate 62: "
                "the inversion flag is one toggle apart from Nuked's, so the "
                "key-off latches the complementary level";
        else if (has_ssg_keyoff_cut_divergence(c.op, eg))
          bad = "SSG-EG key-off with the latched level at or above 0x200: "
                "Nuked holds it for another sample or two before cutting to "
                "0x3FF";
        if (bad) {
          v.ok = false;
          v.why = std::string(bad) + " (sample " + std::to_string(i) +
                  "); see golden/DISCREPANCIES.md";
          return v;
        }
        eg.key_off();
      }
      ++g;
    }
    eg.step();
    if (eg.attenuation() != t.level[i]) {
      v.ok = false;
      v.why = "level mismatch at sample " + std::to_string(i) + ": ours " +
              std::to_string(eg.attenuation()) + " vs Nuked " +
              std::to_string(t.level[i]);
      return v;
    }
    if (i % 3 == 0 && static_cast<int>(eg.phase()) != t.state[i]) {
      v.ok = false;
      v.why = "state mismatch at EG tick " + std::to_string(i) + ": ours " +
              std::to_string(static_cast<int>(eg.phase())) + " vs Nuked " +
              std::to_string(t.state[i]);
      return v;
    }
    const int j = i + kOutputLagSamples;
    if (check_out && j < c.samples) {
      if (eg.output() != t.out[j]) {
        v.ok = false;
        v.why = "output mismatch at sample " + std::to_string(i) + ": ours " +
                std::to_string(eg.output()) + " vs Nuked eg_out[" +
                std::to_string(j) + "] " + std::to_string(t.out[j]);
        return v;
      }
      if (t.out[j] != t.level[i])
        *out_differs = true;
    }
  }
  return v;
}

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

// Emit values[i + skip] as a flat [sample, value, ...] list, one pair per
// change.  `skip` is how many samples of pipeline lag to remove (0 for the raw
// level and state traces, kOutputLagSamples for eg_out), so the list always
// reads as "at sample i the value became v".
template <typename T>
void write_changes(std::string &json, const char *key,
                   const std::vector<T> &values, size_t skip) {
  json += "      \"";
  json += key;
  json += "\": [";
  bool first = true;
  long prev = -1;
  for (size_t i = 0; i + skip < values.size(); ++i) {
    const long v = static_cast<long>(values[i + skip]);
    if (!first && v == prev)
      continue;
    if (!first)
      json += ',';
    json += std::to_string(i);
    json += ',';
    json += std::to_string(v);
    prev = v;
    first = false;
  }
  json += "],\n";
}

std::string case_json(const Case &c, const Trace &t, const Verdict &v,
                      bool out_differs) {
  std::string j;
  j += "    {\n";
  j += "      \"name\": \"" + c.name + "\",\n";
  j += "      \"ar\": " + std::to_string(c.op.ar) +
       ", \"dr\": " + std::to_string(c.op.dr) +
       ", \"sr\": " + std::to_string(c.op.sr) +
       ", \"rr\": " + std::to_string(c.op.rr) +
       ", \"sl\": " + std::to_string(c.op.sl) +
       ", \"tl\": " + std::to_string(c.op.tl) +
       ", \"ks\": " + std::to_string(c.op.ks) +
       ", \"ssg\": " + std::to_string(c.op.ssg) + ",\n";
  j += "      \"fnum\": " + std::to_string(c.pitch.fnum) +
       ", \"block\": " + std::to_string(c.pitch.block) + ",\n";
  j += "      \"samples\": " + std::to_string(c.samples) +
       ", \"counter_phase\": " + std::to_string(t.counter_phase) +
       ", \"counter_shift\": " + std::to_string(v.shift) + ",\n";
  j += "      \"gate\": [";
  for (size_t i = 0; i < t.gate.size(); ++i) {
    if (i)
      j += ',';
    j += std::to_string(t.gate[i].sample);
    j += ',';
    j += t.gate[i].on ? '1' : '0';
  }
  j += "],\n";
  write_changes(j, "level", t.level, 0);
  if (out_differs)
    write_changes(j, "out", t.out, kOutputLagSamples);
  write_changes(j, "state", t.state, 0);
  // Every write_changes() ends in "],\n"; drop the comma of the last one.
  j.erase(j.size() - 2, 1);
  j += "    }";
  return j;
}

bool emit(const Scenario &s, const std::string &dir) {
  std::string j;
  j += "{\n";
  j += "  \"scenario\": \"" + s.file + "\",\n";
  j += "  \"title\": \"" + s.title + "\",\n";
  j += "  \"description\": \"" + s.description + "\",\n";
  j += "  \"source\": \"Nuked-OPN2 ym3438.c @ " + std::string(kNukedCommit) +
       "\",\n";
  j += "  \"format\": \"level/out/state are flat [sample,value,...] change "
       "lists; a value holds until the next pair. out is Nuked eg_out already "
       "shifted back by its 1-sample pipeline lag, and is omitted when it "
       "equals level. gate is [sample,keyed_on,...].\",\n";
  j += "  \"cases\": [\n";

  bool ok = true;
  for (size_t i = 0; i < s.cases.size(); ++i) {
    const Case &c = s.cases[i];
    const Trace t = record(c);
    bool out_differs = false;
    const Verdict v = verify(c, t, &out_differs);
    if (!v.ok) {
      std::fprintf(stderr, "  FAIL %s / %s: %s\n", s.file.c_str(),
                   c.name.c_str(), v.why.c_str());
      ok = false;
      continue;
    }
    if (i)
      j += ",\n";
    j += case_json(c, t, v, out_differs);
  }
  j += "\n  ]\n}\n";

  if (!ok)
    return false;

  const std::string path = dir + "/" + s.file + ".json";
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  std::fwrite(j.data(), 1, j.size(), f);
  std::fclose(f);
  std::printf("  %-20s %2zu cases, %7zu bytes\n", (s.file + ".json").c_str(),
              s.cases.size(), j.size());
  return true;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

NotePitch note(int midi) { return NotePitch::from_midi(midi); }

OperatorParams patch(int ar, int dr, int sr, int rr, int sl, int tl, int ks,
                     int ssg) {
  OperatorParams p;
  p.ar = static_cast<uint8_t>(ar);
  p.dr = static_cast<uint8_t>(dr);
  p.sr = static_cast<uint8_t>(sr);
  p.rr = static_cast<uint8_t>(rr);
  p.sl = static_cast<uint8_t>(sl);
  p.tl = static_cast<uint8_t>(tl);
  p.ks = static_cast<uint8_t>(ks);
  p.ssg = static_cast<uint8_t>(ssg);
  return p;
}

std::vector<Gate> hold(int on, int off) {
  return {{align_gate(on), true}, {align_gate(off), false}};
}

std::string num(int v) { return std::to_string(v); }

std::string hex2(int v) {
  static const char *digits = "0123456789ABCDEF";
  return std::string(1, digits[(v >> 4) & 0xF]) + digits[v & 0xF];
}

// 1. Attack rate sweep: attack shape and the Attack->Decay->Sustain chain.
Scenario ar_sweep() {
  Scenario s;
  s.file = "ar_sweep";
  s.title = "Attack rate sweep";
  s.description = "AR 4..31 step 3 with DR=8 SL=4 SR=3 RR=7, KS=0, C4. "
                  "AR=28 lands on rate 58 and lives in high_rate.json instead.";
  for (int ar : {4, 7, 10, 13, 16, 19, 22, 25, 31})
    s.cases.push_back({"AR=" + num(ar), patch(ar, 8, 3, 7, 4, 0, 0, 0),
                       note(60), 60000, hold(6, 45000)});
  return s;
}

// 2. Decay slope and the sustain transition.
Scenario dr_sl_grid() {
  Scenario s;
  s.file = "dr_sl_grid";
  s.title = "Decay rate x sustain level grid";
  s.description = "DR 4/12/20 x SL 1/4/8/15 with AR=31 SR=6 RR=7, KS=0, C4. "
                  "The DR=28 column lands on rate 58 and lives in "
                  "high_rate.json instead.";
  for (int dr : {4, 12, 20})
    for (int sl : {1, 4, 8, 15})
      s.cases.push_back({"DR=" + num(dr) + " SL=" + num(sl),
                         patch(31, dr, 6, 7, sl, 0, 0, 0), note(60), 40000,
                         hold(6, 30000)});
  return s;
}

// 3. Second decay and release, including SR=0 hold and key-off from every
//    phase.
Scenario sr_rr_sweep() {
  Scenario s;
  s.file = "sr_rr_sweep";
  s.title = "Sustain and release rate sweeps";
  s.description = "SR 0/3/9/15/21 (SR=0 holds forever) and RR 0/3/7/11/15, "
                  "plus key-off taken from attack, decay and sustain.";
  for (int sr : {0, 3, 9, 15, 21})
    s.cases.push_back({"SR=" + num(sr), patch(31, 20, sr, 7, 4, 0, 0, 0),
                       note(60), 40000, hold(6, 30000)});
  for (int rr : {0, 3, 7, 11, 15})
    s.cases.push_back({"RR=" + num(rr), patch(31, 20, 0, rr, 2, 0, 0, 0),
                       note(60), 40000, hold(6, 12000)});
  // AR=12 -> rate 26 -> ~3100 EG ticks -> the attack spans ~9300 samples.
  s.cases.push_back({"key-off in attack", patch(12, 8, 4, 6, 6, 0, 0, 0),
                     note(60), 30000, hold(6, 5000)});
  s.cases.push_back({"key-off in decay", patch(12, 8, 4, 6, 6, 0, 0, 0),
                     note(60), 30000, hold(6, 12000)});
  s.cases.push_back({"key-off in sustain", patch(12, 20, 4, 6, 2, 0, 0, 0),
                     note(60), 30000, hold(6, 20000)});
  return s;
}

// 4. Key scaling: the same registers at three octaves and four KS settings.
Scenario ks_pitch() {
  Scenario s;
  s.file = "ks_pitch";
  s.title = "Key scaling x pitch";
  s.description = "KS 0..3 x C1/C4/C7 for one patch (AR=9 DR=8 SR=6 RR=4 "
                  "SL=3), chosen so even KS=3 at C7 stays below rate 48.";
  for (int ks = 0; ks < 4; ++ks)
    for (int midi : {24, 60, 96})
      s.cases.push_back({"KS=" + num(ks) + " midi=" + num(midi),
                         patch(9, 8, 6, 4, 3, 0, ks, 0), note(midi), 40000,
                         hold(6, 30000)});
  return s;
}

// 5. All eight SSG-EG shapes.
Scenario ssg_modes() {
  Scenario s;
  s.file = "ssg_modes";
  s.title = "SSG-EG shapes";
  s.description = "All 8 SSG-EG modes ($08..$0F) with AR=31, two DR/SL/SR "
                  "combinations each, plus late key-offs that fall inside an "
                  "inverted ramp to exercise the release latch.";
  for (int ssg = 8; ssg < 16; ++ssg) {
    s.cases.push_back({"SSG=$" + hex2(ssg) + " slow",
                       patch(31, 12, 8, 6, 15, 0, 0, ssg), note(60), 24000,
                       hold(6, 17000)});
    s.cases.push_back({"SSG=$" + hex2(ssg) + " fast",
                       patch(31, 20, 10, 4, 4, 0, 0, ssg), note(60), 24000,
                       hold(6, 15000)});
    // A key-off placed well inside the second ramp of the alternating modes,
    // i.e. while the output inversion is active.
    s.cases.push_back({"SSG=$" + hex2(ssg) + " key-off inverted",
                       patch(31, 18, 14, 5, 8, 0, 0, ssg), note(60), 20000,
                       hold(6, 9000)});
  }
  return s;
}

// 6. SSG-EG with AR below 31.  The attack then starts from 0x3FF, which is
//    above the fold level, so the SSG block acts on every sample of the attack
//    itself; only the instant attack skips that region entirely.
Scenario ssg_slow_attack() {
  Scenario s;
  s.file = "ssg_slow_attack";
  s.title = "SSG-EG under a slow attack";
  s.description = "SSG-EG with AR 0/3/10, so the whole attack sits at or above "
                  "the 0x200 fold. Every case runs past the first fold that "
                  "follows its attack. The alternating modes without hold "
                  "($0A and $0E) are never keyed off; see "
                  "golden/DISCREPANCIES.md.";
  // Modes $0A and $0E hold the inversion flag one toggle away from Nuked's for
  // the whole of a sub-62 attack, and only a key-off can carry that into
  // eg_level, so those cases end while still keyed on.
  const std::vector<Gate> on_only = {{align_gate(6), true}};

  // The attack alone spans ~222700 samples at AR=3 and ~18600 at AR=10, and
  // the climb back to the fold a further ~86800; `samples` has to cover both
  // for the trace to reach the fold at all.
  s.cases.push_back({"AR=3 SSG=$08", patch(3, 10, 6, 5, 7, 0, 0, 8), note(60),
                     330000, hold(6, 318000)});
  s.cases.push_back({"AR=3 SSG=$0A", patch(3, 10, 6, 5, 7, 0, 0, 10), note(60),
                     330000, on_only});
  s.cases.push_back({"AR=10 SSG=$08", patch(10, 10, 6, 5, 7, 0, 0, 8), note(60),
                     126000, hold(6, 114000)});
  s.cases.push_back({"AR=10 SSG=$0A", patch(10, 10, 6, 5, 7, 0, 0, 10),
                     note(60), 126000, on_only});
  // Rate 0 never advances, so the level stays pinned above the fold for the
  // whole note while the alternate bit flips the inversion every sample.
  for (int ssg : {10, 14})
    s.cases.push_back({"AR=0 SSG=$" + hex2(ssg),
                       patch(0, 10, 6, 5, 7, 0, 0, ssg), note(60), 12000,
                       on_only});
  // SL=15 sits above the fold, so the decay runs straight into it: the first
  // fold lands at sample 19638 and the modes without hold take a second one at
  // 36723, well inside the trace.
  for (int ssg = 8; ssg < 16; ++ssg) {
    const bool alternating = (ssg & 0x02) && !(ssg & 0x01);
    s.cases.push_back({"SSG=$" + hex2(ssg) + " AR=10 fold cycle",
                       patch(10, 20, 10, 5, 15, 0, 0, ssg), note(60), 60000,
                       alternating ? on_only : hold(6, 50000)});
  }
  return s;
}

// 7. Retrigger.
Scenario retrigger() {
  Scenario s;
  s.file = "retrigger";
  s.title = "Retrigger";
  s.description = "Key-off then key-on again part-way through the release, so "
                  "the attack resumes from the level it finds.";
  s.cases.push_back({"retrigger mid-release", patch(16, 12, 6, 5, 4, 0, 0, 0),
                     note(60), 40000,
                     {{align_gate(6), true},
                      {align_gate(12000), false},
                      {align_gate(14000), true},
                      {align_gate(30000), false}}});
  s.cases.push_back({"retrigger, slow attack", patch(8, 14, 6, 8, 5, 0, 0, 0),
                     note(60), 60000,
                     {{align_gate(6), true},
                      {align_gate(20000), false},
                      {align_gate(24000), true},
                      {align_gate(50000), false}}});
  s.cases.push_back({"retrigger, instant attack",
                     patch(31, 14, 6, 5, 6, 0, 0, 0), note(60), 40000,
                     {{align_gate(6), true},
                      {align_gate(9000), false},
                      {align_gate(11000), true},
                      {align_gate(28000), false}}});
  s.cases.push_back({"retrigger with SSG=$0A", patch(31, 16, 10, 6, 4, 0, 0, 10),
                     note(60), 30000,
                     {{align_gate(6), true},
                      {align_gate(9000), false},
                      {align_gate(11000), true},
                      {align_gate(22000), false}}});
  return s;
}

// 8. Edge anchors.
Scenario edge_anchors() {
  Scenario s;
  s.file = "edge_anchors";
  s.title = "Edge anchors";
  s.description = "SL=0 skip-decay (with a real attack -- SL=0 plus the "
                  "instant attack is a documented divergence), SL=15, instant "
                  "attack, AR=0, DR=0, SR=0, and TL well above zero.";
  s.cases.push_back({"SL=0 skip decay", patch(16, 10, 5, 7, 0, 0, 0, 0),
                     note(60), 40000, hold(6, 25000)});
  s.cases.push_back({"SL=0, KS=2", patch(14, 12, 7, 6, 0, 0, 2, 0), note(60),
                     40000, hold(6, 25000)});
  s.cases.push_back({"SL=15", patch(31, 10, 5, 7, 15, 0, 0, 0), note(60), 60000,
                     hold(6, 45000)});
  s.cases.push_back({"instant attack (AR=31)", patch(31, 14, 6, 7, 4, 0, 0, 0),
                     note(60), 30000, hold(6, 20000)});
  s.cases.push_back({"AR=0 (attack frozen)", patch(0, 10, 5, 7, 4, 0, 0, 0),
                     note(60), 20000, hold(6, 12000)});
  s.cases.push_back({"DR=0 (parked at 0)", patch(31, 0, 5, 7, 4, 0, 0, 0),
                     note(60), 20000, hold(6, 12000)});
  s.cases.push_back({"SR=0 (holds at SL)", patch(31, 20, 0, 7, 4, 0, 0, 0),
                     note(60), 20000, hold(6, 12000)});
  s.cases.push_back({"TL=40", patch(31, 12, 6, 7, 5, 40, 0, 0), note(60), 30000,
                     hold(6, 20000)});
  s.cases.push_back({"TL=100", patch(20, 12, 6, 7, 5, 100, 0, 0), note(60),
                     30000, hold(6, 20000)});
  s.cases.push_back({"TL=64, SSG=$0C", patch(31, 16, 9, 6, 4, 64, 0, 12),
                     note(60), 24000, hold(6, 15000)});
  return s;
}

// 9. The rate >= 48 regime, where Nuked's latched timer bits rotate the
//    increment row by one EG tick.  Every case here keeps all of its
//    non-constant rows on the same side of that rotation, so the comparison
//    stays exact once the documented counter shift is applied.
Scenario high_rate() {
  Scenario s;
  s.file = "high_rate";
  s.title = "Rates >= 48 (eg_timer_low_lock rotation)";
  s.description = "Cases whose non-constant increment rows are all >= 48. The "
                  "library follows the published table, Nuked latches the "
                  "timer's low bits one tick late, so these are compared with "
                  "the documented one-EG-tick counter shift -- exactly, not "
                  "with a tolerance. See golden/DISCREPANCIES.md.";
  // ksv = 2 at C4/KS=0, so every rate is 0 or 2 mod 4: the 2 mod 4 rows all
  // want the same -1 shift and the 0 mod 4 rows are constant.
  s.cases.push_back({"AR=28 (rate 58)", patch(28, 26, 24, 15, 4, 0, 0, 0),
                     note(60), 11000, hold(6, 7000)});
  s.cases.push_back({"AR=26 (rate 54)", patch(26, 26, 24, 15, 4, 0, 0, 0),
                     note(60), 11000, hold(6, 7000)});
  for (int sl : {1, 4, 8, 15})
    s.cases.push_back({"DR=28 SL=" + num(sl), patch(31, 28, 26, 15, sl, 0, 0, 0),
                       note(60), 11000, hold(6, 7000)});
  s.cases.push_back({"SR=24 (rate 50)", patch(31, 28, 24, 15, 2, 0, 0, 0),
                     note(60), 11000, hold(6, 7000)});
  // SL=15 keeps the SSG ramp under DR the whole way, so the 4x step never has
  // a Decay->Sustain window to jump.
  s.cases.push_back({"SSG=$08 at rate 58", patch(31, 28, 26, 15, 15, 0, 0, 8),
                     note(60), 11000, hold(6, 7000)});
  s.cases.push_back({"SSG=$0A at rate 58", patch(31, 28, 26, 15, 15, 0, 0, 10),
                     note(60), 11000, hold(6, 7000)});
  // An odd keycode (F-num bit 10 region) makes ksv odd, which is the only way
  // to reach rate % 4 == 1 and 3.  fnum 960 -> fn_note 1, block 6 -> kcode 25,
  // KS=3 -> ksv 25.
  const NotePitch odd{960, 6};
  s.cases.push_back({"rates 51/55/59 (+1 shift)",
                     patch(13, 15, 17, 7, 4, 0, 3, 0), odd, 11000,
                     hold(6, 7000)});
  s.cases.push_back({"rates 49/53/57 (-1 shift)",
                     patch(12, 14, 16, 10, 4, 0, 3, 0), odd, 11000,
                     hold(6, 7000)});
  s.cases.push_back({"rates 51/55/59, SL=1", patch(13, 15, 17, 7, 1, 0, 3, 0),
                     odd, 11000, hold(6, 7000)});
  return s;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  std::printf("golden vectors from Nuked-OPN2 @ %s\n", kNukedCommit);

  const Scenario scenarios[] = {ar_sweep(),        dr_sl_grid(),
                                sr_rr_sweep(),     ks_pitch(),
                                ssg_modes(),       ssg_slow_attack(),
                                retrigger(),       edge_anchors(),
                                high_rate()};
  bool ok = true;
  for (const Scenario &s : scenarios)
    ok &= emit(s, dir);
  if (!ok) {
    std::fprintf(stderr, "\ngeneration FAILED: at least one case does not "
                         "reproduce on EgSimulator\n");
    return 1;
  }
  std::printf("all scenarios verified against EgSimulator\n");
  return 0;
}
