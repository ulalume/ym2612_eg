#pragma once

// ym2612_eg - sample-accurate YM2612 (OPN2) envelope generator simulator
// for visualisation.  Header-only, C++17, no dependencies.
//
// `sample_curve()` returns a ready-to-draw curve; `EgSimulator` steps one
// operator sample by sample; `phase_durations()` and `ssg_loop_period_ms()`
// answer how long the shape takes without simulating it.  Where the reference
// emulators disagree, this follows Nuked-OPN2.
//
// `ym2612_eg::graph` sits on top of all of it: how wide a time axis an
// envelope deserves, the traces to draw on it, where a sounding voice is, and
// the handles the shape is dragged by.

#include "detail/constants.hpp"
#include "detail/curve.hpp"
#include "detail/simulator.hpp"
#include "detail/tables.hpp"
#include "detail/timing.hpp"
#include "graph.hpp"
#include "handles.hpp"
