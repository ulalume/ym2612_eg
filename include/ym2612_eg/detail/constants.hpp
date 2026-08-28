#pragma once

// Clocks, fixed-point conventions and unit conversions.

#include <cmath>
#include <cstdint>

namespace ym2612_eg {

// Genesis / Mega Drive master clocks.
inline constexpr double kNtscClockHz = 53693175.0 / 7.0; // 7670453.57 Hz
inline constexpr double kPalClockHz = 53203424.0 / 7.0;  // 7600489.14 Hz

// The chip runs 24 operator slots x 6 cycles, so one output sample every
// 144 master clocks.  The envelope generator advances once per 3 samples.
inline constexpr int kSampleDivider = 144;
inline constexpr int kEgClockDivider = 3;

// 10-bit attenuation, 4.6 fixed point.  0 = loudest.
inline constexpr uint16_t kMaxAttenuation = 0x3FF;
// SSG-EG folds / freezes here.
inline constexpr uint16_t kSsgFoldAttenuation = 0x200;
// Any output attenuation at or above this multiplies to exactly 0 in hardware.
inline constexpr uint16_t kSilenceAttenuation = 0x340;

inline constexpr double sample_rate_hz(double clock_hz) {
  return clock_hz / kSampleDivider;
}

inline constexpr double eg_rate_hz(double clock_hz) {
  return clock_hz / (kSampleDivider * kEgClockDivider);
}

// One attenuation unit is 20*log10(2)/64 = 0.0940719 dB.
inline double atten_to_db(uint16_t a) {
  return static_cast<double>(a) * (20.0 * 0.301029995663981195 / 64.0);
}

// Linear amplitude multiplier the operator would be scaled by.
inline double atten_to_amplitude(uint16_t a) {
  return std::exp2(-static_cast<double>(a) / 64.0);
}

} // namespace ym2612_eg
