#pragma once

// A second, deliberately independent implementation of the base (non-SSG)
// envelope generator, written as an EG-tick loop straight from the pseudocode
// in EG_SPEC.md ("Reference tick loop") rather than as a per-sample machine.
// Used to cross-check EgSimulator, and to produce the ymfm-style numbers for
// the anchors that predate Nuked's "envelope off" snap.

#include "ym2612_eg/ym2612_eg.hpp"

#include <cstdint>

namespace refeg {

enum { kA = 0, kD = 1, kS = 2, kR = 3 };

struct Reference {
  // Configuration.
  int ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, ks = 0, tl = 0;
  int keycode = 0;
  bool snap = true; // Nuked "envelope off" snap

  // State.
  int rate[4] = {0, 0, 0, 0};
  int sustain = 0;
  int att = 0x3FF;
  int counter = 0;
  int state = kR;
  bool keyed = false;
  uint32_t ticks = 0;

  void configure() {
    const int ksv = keycode >> (3 - ks);
    rate[kA] = ar == 0 ? 0 : (2 * ar + ksv > 63 ? 63 : 2 * ar + ksv);
    rate[kD] = dr == 0 ? 0 : (2 * dr + ksv > 63 ? 63 : 2 * dr + ksv);
    rate[kS] = sr == 0 ? 0 : (2 * sr + ksv > 63 ? 63 : 2 * sr + ksv);
    const int rrel = 2 * rr + 1;
    rate[kR] = 2 * rrel + ksv > 63 ? 63 : 2 * rrel + ksv;
    sustain = (sl | ((sl + 1) & 0x10)) << 5;
  }

  void key_on() {
    if (keyed)
      return;
    keyed = true;
    state = kA;
    if (rate[kA] >= 62)
      att = 0;
  }

  void key_off() {
    if (!keyed)
      return;
    keyed = false;
    state = kR;
  }

  void tick() {
    ++ticks;
    counter = (counter + 1) & 0xFFF;
    if (counter == 0)
      counter = 1;

    if (state == kA && att == 0)
      state = kD;
    if (state == kD && att >= sustain)
      state = kS;

    const int r = rate[state];
    const int shift = r >= 44 ? 0 : 11 - (r >> 2);
    if (counter & ((1 << shift) - 1))
      return;
    const int inc = ym2612_eg::detail::kIncTable[r][(counter >> shift) & 7];

    if (state == kA) {
      if (r < 62 && inc != 0)
        att += (~att * inc) >> 4;
      return;
    }
    att += inc;
    if (att > 0x3FF)
      att = 0x3FF;
    if (snap && (att & 0x3F0) == 0x3F0) {
      att = 0x3FF;
      state = kR;
    }
  }

  int output() const {
    const int o = att + (tl << 3);
    return o > 0x3FF ? 0x3FF : o;
  }
};

} // namespace refeg
