#pragma once

// The 64x8 envelope increment table and the effective-rate arithmetic.

#include <cstdint>

namespace ym2612_eg {
namespace detail {

// Rows 0/1 are all-zero: rate 0 never advances, so R=0 holds forever.
// Rows 2-7 are degenerate because shift >= 10 leaves fewer than 3 usable
// index bits in the 12-bit counter.  Rows 8-47 repeat with period 4.
inline constexpr uint8_t kIncTable[64][8] = {
    /* 0*/ {0, 0, 0, 0, 0, 0, 0, 0},
    /* 1*/ {0, 0, 0, 0, 0, 0, 0, 0},
    /* 2*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /* 3*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /* 4*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /* 5*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /* 6*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /* 7*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /* 8*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /* 9*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*10*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*11*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*12*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*13*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*14*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*15*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*16*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*17*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*18*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*19*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*20*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*21*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*22*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*23*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*24*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*25*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*26*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*27*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*28*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*29*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*30*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*31*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*32*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*33*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*34*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*35*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*36*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*37*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*38*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*39*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*40*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*41*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*42*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*43*/ {0, 1, 1, 1, 1, 1, 1, 1},
    /*44*/ {0, 1, 0, 1, 0, 1, 0, 1},
    /*45*/ {0, 1, 0, 1, 1, 1, 0, 1},
    /*46*/ {0, 1, 1, 1, 0, 1, 1, 1},
    /*47*/ {0, 1, 1, 1, 1, 1, 1, 1},
    // Rows 48-63 spell out kStepHi below, so they repeat every four entries.
    /*48*/ {1, 1, 1, 1, 1, 1, 1, 1},
    /*49*/ {2, 1, 1, 1, 2, 1, 1, 1},
    /*50*/ {2, 1, 2, 1, 2, 1, 2, 1},
    /*51*/ {2, 2, 2, 1, 2, 2, 2, 1},
    /*52*/ {2, 2, 2, 2, 2, 2, 2, 2},
    /*53*/ {4, 2, 2, 2, 4, 2, 2, 2},
    /*54*/ {4, 2, 4, 2, 4, 2, 4, 2},
    /*55*/ {4, 4, 4, 2, 4, 4, 4, 2},
    /*56*/ {4, 4, 4, 4, 4, 4, 4, 4},
    /*57*/ {8, 4, 4, 4, 8, 4, 4, 4},
    /*58*/ {8, 4, 8, 4, 8, 4, 8, 4},
    /*59*/ {8, 8, 8, 4, 8, 8, 8, 4},
    /*60*/ {8, 8, 8, 8, 8, 8, 8, 8},
    /*61*/ {8, 8, 8, 8, 8, 8, 8, 8},
    /*62*/ {8, 8, 8, 8, 8, 8, 8, 8},
    /*63*/ {8, 8, 8, 8, 8, 8, 8, 8},
};

// At rate >= 48 the step is picked by the counter's low two bits:
// 1 << (min(kStepHi[rate & 3][counter & 3] + (rate >> 2) - 11, 4) - 1).
inline constexpr uint8_t kStepHi[4][4] = {
    {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};

inline constexpr bool high_rows_follow_step_hi() {
  for (int rate = 48; rate < 64; ++rate)
    for (int i = 0; i < 8; ++i) {
      const int e = kStepHi[rate & 3][i & 3] + (rate >> 2) - 11;
      if (kIncTable[rate][i] != (1 << ((e < 4 ? e : 4) - 1)))
        return false;
    }
  return true;
}
static_assert(high_rows_follow_step_hi(), "rows 48-63 must follow kStepHi");

// Rates >= 44 must all give shift 0.
inline constexpr int rate_shift(int rate) {
  const int s = 11 - (rate >> 2);
  return s > 0 ? s : 0;
}

// R == 0 ignores ksv entirely; that is how SR=0 holds forever.
inline constexpr int effective_rate(int raw_r, int ksv) {
  if (raw_r == 0)
    return 0;
  const int r = 2 * raw_r + ksv;
  return r > 63 ? 63 : r;
}

// Increment for this rate at this 12-bit counter value.
inline constexpr int increment_at(int rate, int counter) {
  const int shift = rate_shift(rate);
  if (counter & ((1 << shift) - 1))
    return 0;
  return kIncTable[rate][(counter >> shift) & 7];
}

} // namespace detail
} // namespace ym2612_eg
