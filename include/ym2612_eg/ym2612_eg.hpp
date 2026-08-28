#pragma once

// ym2612_eg - sample-accurate YM2612 (OPN2) envelope generator simulator
// for visualisation.  Header-only, C++17, no dependencies.
//
// `sample_curve()` returns a ready-to-draw curve; `EgSimulator` steps one
// operator sample by sample.  Where the reference emulators disagree, this
// follows Nuked-OPN2.

#include "detail/constants.hpp"
#include "detail/curve.hpp"
#include "detail/simulator.hpp"
#include "detail/tables.hpp"
