#pragma once

// ym2612_eg - sample-accurate YM2612 (OPN2) envelope generator simulator
// for visualisation.  Header-only, C++17, no dependencies.
//
//   #include <ym2612_eg/ym2612_eg.hpp>
//
//   ym2612_eg::CurveRequest req;
//   req.op    = {31, 10, 5, 7, 2, 0, 0, 0};   // AR DR SR RR SL TL KS SSG
//   req.pitch = ym2612_eg::NotePitch::from_midi(60);
//   req.gate_ms = 500.0;
//   req.max_ms  = 2000.0;
//   const auto curve = ym2612_eg::sample_curve(req);
//
// Or drive one operator sample by sample:
//
//   ym2612_eg::EgSimulator eg(req.op, req.pitch);
//   eg.key_on();
//   eg.step(1000);
//   eg.output();
//
// Behaviour is specified by EG_SPEC.md and SSG_EG_SPEC.md.  Where the
// reference emulators disagree we follow Nuked-OPN2, including its
// "envelope off" snap; those spots are commented `// Nuked behavior`.

#include "detail/constants.hpp"
#include "detail/curve.hpp"
#include "detail/simulator.hpp"
#include "detail/tables.hpp"
