#pragma once

// A second, deliberately independent implementation of the base (non-SSG)
// envelope generator, written as an EG-tick loop rather than as a per-sample
// machine.  One tick() is the tick's own output sample and the two after it:
// a key edge, a phase transition and the "envelope off" snap each take one of
// those samples, and only the tick's own sample adds an increment.  Used to
// cross-check EgSimulator, and with `snap` off to produce the ymfm-style
// numbers for the anchors that predate Nuked's "envelope off" snap.

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
  bool seen = false; // the key state the envelope has acted on
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

  // Edge-triggered; the edge reaches the envelope on the next tick.
  void key_on() { keyed = true; }
  void key_off() { keyed = false; }

  void tick() {
    ++ticks;
    counter = (counter + 1) & 0xFFF;
    if (counter == 0)
      counter = 1;

    // The tick's own sample.
    const bool key_on_edge = keyed && !seen;
    seen = keyed;
    if (key_on_edge) {
      state = kA;
      if (rate[kA] >= 62)
        att = 0;
    } else if (!settle()) {
      const int r = rate[state];
      const int shift = r >= 44 ? 0 : 11 - (r >> 2);
      const int inc =
          (counter & ((1 << shift) - 1))
              ? 0
              : ym2612_eg::detail::kIncTable[r][(counter >> shift) & 7];
      if (state == kA) {
        if (keyed && r < 62)
          att += (~att * inc) >> 4;
      } else {
        att += inc;
        if (att > 0x3FF)
          att = 0x3FF;
      }
      if (!keyed)
        state = kR;
    }
    // The two samples before the next tick.
    settle();
    settle();
  }

  // One output sample off the tick: the snap or the transition that is due,
  // if any.  Returns whether one was.
  bool settle() {
    if (snap && state != kA && (att & 0x3F0) == 0x3F0 &&
        !(state == kR && att == 0x3FF)) {
      att = 0x3FF;
      state = kR;
      return true;
    }
    if (state == kA && att == 0) {
      state = keyed ? kD : kR;
      return true;
    }
    if (state == kD && att >= sustain) {
      state = keyed ? kS : kR;
      return true;
    }
    return false;
  }

  int output() const {
    const int o = att + (tl << 3);
    return o > 0x3FF ? 0x3FF : o;
  }
};

} // namespace refeg
