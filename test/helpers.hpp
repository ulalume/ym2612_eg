#pragma once

#include "ym2612_eg/ym2612_eg.hpp"

#include <cstdint>
#include <vector>

namespace helpers {

using ym2612_eg::EgPhase;
using ym2612_eg::EgSimulator;

// One EG tick is three output samples, and the tick runs on the first of the
// three, so after step(3) the simulator state reflects exactly one more tick.
inline void eg_tick(EgSimulator &s) { s.step(3); }

// Runs up to max_ticks EG ticks; returns the 1-based tick index at which
// pred(s) first held, or 0 if it never did.
template <typename Pred>
inline uint32_t ticks_until(EgSimulator &s, Pred pred, uint32_t max_ticks) {
  for (uint32_t t = 1; t <= max_ticks; ++t) {
    eg_tick(s);
    if (pred(s))
      return t;
  }
  return 0;
}

inline double ticks_to_ms(double ticks,
                          double clock_hz = ym2612_eg::kNtscClockHz) {
  return ticks * 1000.0 / ym2612_eg::eg_rate_hz(clock_hz);
}

// Sample indices at which the SSG block folds: the one sample observed at
// A >= 0x200 just before the state change (SSG_EG_SPEC 2b).
inline std::vector<uint64_t> ssg_fold_samples(EgSimulator &s, size_t want,
                                              uint64_t max_samples) {
  std::vector<uint64_t> folds;
  folds.reserve(want);
  for (uint64_t i = 0; i < max_samples && folds.size() < want; ++i) {
    if (s.attenuation() >= ym2612_eg::kSsgFoldAttenuation)
      folds.push_back(i);
    s.step();
  }
  return folds;
}

// Mean fold-to-fold interval in ms, skipping the first interval (it carries
// the initial ramp).  Matches ssg_sim.py's averaging.
inline double mean_ramp_ms(const std::vector<uint64_t> &folds,
                           double clock_hz = ym2612_eg::kNtscClockHz) {
  if (folds.size() < 3)
    return 0.0;
  double sum = 0.0;
  size_t n = 0;
  for (size_t k = 2; k < folds.size(); ++k) {
    sum += static_cast<double>(folds[k] - folds[k - 1]);
    ++n;
  }
  return (sum / static_cast<double>(n)) * 1000.0 /
         ym2612_eg::sample_rate_hz(clock_hz);
}

// Deterministic little LCG so the randomised cross-check is reproducible.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ULL + 1) {}
  uint32_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(s >> 33);
  }
  int in(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

} // namespace helpers
