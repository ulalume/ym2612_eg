# ym2612_eg

A sample-accurate YM2612 / OPN2 envelope generator **simulator** for
visualisation. Header-only C++17, no dependencies.

This is not an audio emulator: there is no phase generator, no sine/exp
tables, no channel mixing. Everything that changes the *shape* of an
operator's envelope is modelled exactly, at the chip's own sample rate.

## What it models

| Area | Detail |
| ---- | ------ |
| Timing | `clock / 144` output samples, `/3` envelope divider, 12-bit free-running counter that skips 0 on overflow |
| Rates | `keycode` from block + F-num, `ksv = keycode >> (3 - KS)`, `rate = min(2R + ksv, 63)`, `R = 0` holds forever |
| Attack | `att += (~att * inc) >> 4`, instant attack at rate >= 62, freeze at rates 0/1 and 62/63 |
| Decay / sustain / release | 64x8 increment table, `SL = 15 -> 0x3E0`, transitions checked at the top of the tick so `SL = 0` skips decay |
| SSG-EG | Full per-sample state machine: 4x increments, freeze at `0x200`, output inversion, invert-flag lifecycle, virtual key-on, key-off latch and hard cut, all 8 shapes |
| Output | `min((J ? 0x200 - A : A) + TL * 8, 0x3FF)` |

Where the reference emulators disagree, Nuked-OPN2 wins, including its
"envelope off" snap (`(att & 0x3F0) == 0x3F0` -> `att = 0x3FF`, state
`Release`). Those spots are commented `// Nuked behavior`.

Not modelled (draw them as separate overlays if you need them): LFO AM /
tremolo, the phase generator itself, algorithm and feedback routing.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The library itself is header-only, so `cmake` is only needed for the tests
and the benchmark.

### Options

| Variable                | Default | Description                     |
| ----------------------- | ------- | ------------------------------- |
| `YM2612_EG_BUILD_TESTS` | `OFF`   | Build the CTest suite           |
| `YM2612_EG_BUILD_BENCH` | `OFF`   | Build the `sample_curve` timing |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DYM2612_EG_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Library usage

```cpp
#include <ym2612_eg/ym2612_eg.hpp>

using namespace ym2612_eg;
```

### One curve, ready to draw

```cpp
CurveRequest req;
req.op      = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0}; // AR DR SR RR SL TL KS SSG
req.pitch   = NotePitch::from_midi(60);
req.gate_ms = 500.0;   // < 0 means "held forever", no release segment
req.max_ms  = 2000.0;

const CurveResult curve = sample_curve(req);

for (const CurvePoint &p : curve.points)
  plot(p.ms, atten_to_db(p.out));       // p.att is the internal trace
for (const Marker &m : curve.markers)
  annotate(m.ms, m.kind);               // AttackEnd, KeyOff, SsgFold, Park, ...
for (CurveWarning w : curve.warnings)
  warn(w);                              // SsgNeverLoops, SsgAudioRate, ...
```

`points` carries one vertex per change in `out` or `att`, decimated so that
the drawn polyline never moves by a whole attenuation unit. Simulation stops
early once nothing more can happen (`park_ms`), or after five SSG loop periods
(`loop_hz`).

### Driving one operator by hand

```cpp
EgSimulator eg(req.op, req.pitch);   // third argument selects the clock
eg.key_on();
eg.step(1000);                       // 1000 output samples
eg.attenuation();                    // internal A, 0..0x3FF
eg.output();                         // after SSG inversion and TL
eg.phase();                          // Attack / Decay / Sustain / Release
eg.is_static();                      // nothing can change without an event
eg.time_ms();
```

`set_params()` and `set_pitch()` recompute the effective rates immediately,
which is what a mid-note register write does on hardware. `reset()` takes an
optional 12-bit counter phase (the counter is free-running and shared by all
24 operators, so the phase at key-on jitters the first update by up to one
period) and an optional starting attenuation for retrigger visualisation.

### Pitch

`NotePitch::from_midi()` reproduces megatoy's note table exactly, so an
envelope drawn here matches what megatoy plays. `NotePitch{fnum, block}` works
too if you already have raw register values.

## CMake integration

```cmake
include(FetchContent)
FetchContent_Declare(ym2612_eg
  GIT_REPOSITORY https://github.com/ulalume/ym2612_eg.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(ym2612_eg)
target_link_libraries(your_target PRIVATE ym2612_eg)
```

or simply add `include/` to your include path.

## Performance

`sample_curve()` over a 10 second span of a 6.5 Hz SSG loop (532 670 samples,
5705 output points) takes about **4-7 ms** in a Release build on an Apple
silicon laptop; a plain ADSR patch over the same span takes about 3 ms. The
target was "well under 50 ms".

## Accuracy

Every numeric anchor in the two specifications is encoded as a test: the
attack trajectory and its 73/40/21/10 update counts, the worked example's
5440-tick decay and 488 585-tick sustain, the KS sweep and three-octave
tables, the attack-shape anchors, the SSG loop-period tables against DR and
SR, all eight SSG shapes, the key-off latch and hard cut, and the decimation
error bound. The base envelope is additionally cross-checked tick-by-tick
against a second, independently written EG-tick-driven model over 120
randomised patches.

## License

MIT.
