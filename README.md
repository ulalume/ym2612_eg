# ym2612_eg

A sample-accurate YM2612 / OPN2 envelope generator simulator for visualisation.
Header-only C++17, no dependencies.

Not an audio emulator: no phase generator, no sine/exp tables, no channel
mixing. Everything that changes the shape of an operator's envelope is
modelled at the chip's own sample rate.

| Area | Detail |
| ---- | ------ |
| Timing | `clock / 144` output samples, `/3` envelope divider, 12-bit counter that skips 0 on overflow |
| Rates | `ksv = keycode >> (3 - KS)`, `rate = min(2R + ksv, 63)`, `R = 0` holds forever |
| Attack | `att += (~att * inc) >> 4`, instant at rate >= 62, frozen at rates 0/1 and 62/63 |
| Decay / sustain / release | 64x8 increment table, `SL = 15 -> 0x3E0`, transitions at the top of the tick so `SL = 0` skips decay |
| SSG-EG | Per-sample state machine: 4x increments, freeze at `0x200`, output inversion, virtual key-on, key-off latch and hard cut, all 8 shapes |
| Durations | Closed-form phase lengths and SSG loop period, `infinity` where a rate never advances |
| Output | `min((J ? 0x200 - A : A) + TL * 8, 0x3FF)` |

LFO AM, the phase generator and algorithm routing are out of scope; draw them
as separate overlays if you need them.

## Usage

```cpp
#include <ym2612_eg/ym2612_eg.hpp>
using namespace ym2612_eg;

CurveRequest req;
req.op      = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0}; // AR DR SR RR SL TL KS SSG
req.pitch   = NotePitch::from_midi(60);
req.gate_ms = 500.0;   // < 0 holds forever and draws no release
req.max_ms  = 2000.0;

const CurveResult curve = sample_curve(req);

for (const CurvePoint &p : curve.points)
  plot(p.ms, atten_to_db(p.out));   // p.att is the internal trace
for (const Marker &m : curve.markers)
  annotate(m.ms, m.kind);           // AttackEnd, KeyOff, SsgFold, Park, ...
for (CurveWarning w : curve.warnings)
  warn(w);                          // SsgNeverLoops, SsgAudioRate, ...
```

`points` carries one vertex per change in `out` or `att`, decimated to stay
within one attenuation unit of the full trace. Sampling stops once nothing
more can happen (`park_ms`) or after five SSG loop periods (`loop_hz`).

## How long does it take?

A caller sizing a time axis needs the durations *before* it knows how far to
simulate, and "how far did you look?" is a place for an answer to change
discontinuously: a rate of 0 really does hold forever, and a run that gave up
cannot tell `SR = 0` from `SR = 31`. So the durations are closed forms, and
`infinity` means the phase never advances.

```cpp
const PhaseDurations d = phase_durations(req.op, req.pitch);
d.attack_ms; d.decay_ms; d.sustain_ms;   // sustain level -> silence
d.sustain_start_ms();                    // attack + decay
d.lifetime_ms();                         // key-on -> silence

ssg_loop_period_ms(req.op, req.pitch);   // 0 = the mode does not loop
                                         // inf = a rate the ramp needs stalls
```

Both take an optional `clock_hz`. The post-attack phases are linear in
attenuation, so each is one division; the attack recurrence and the SSG ramp
are walked instead, a few hundred table slots rather than the hundreds of
thousands of samples the same stretch costs to simulate. `test/timing_test.cpp`
cross-checks both against `sample_curve()` over a few thousand patches: the
decay end and the lifetime land within 2%, an SSG loop period within 3.2%
(0.7% at `AR = 31`). The one weak corner is `SL = 15`, whose sustain phase is
sixteen attenuation units wide -- a single increment of the decay's overshoot
is then a large fraction of it.

Three register-derived values come with them, because a caller cannot ask the
questions above without them:

```cpp
key_scale_value(op, pitch);   // keycode >> (3 - KS): the only route a note
                              // takes into an envelope, so two notes sharing
                              // it have bit-identical curves
loudest_attenuation(op);      // 0, or 0x200 for the inverted SSG-EG modes:
                              // where a release starts from
sustain_attenuation(sl);      // SL -> attenuation; SL = 15 is 0x3E0
```

To drive one operator directly:

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

`set_params()` and `set_pitch()` recompute the effective rates immediately, as
a mid-note register write does. `reset()` takes an optional 12-bit counter
phase and an optional starting attenuation for retriggers.

`NotePitch::from_midi()` follows megatoy's note table; `NotePitch{fnum, block}`
takes raw register values.

## CMake

```cmake
include(FetchContent)
FetchContent_Declare(ym2612_eg
  GIT_REPOSITORY https://github.com/ulalume/ym2612_eg.git
  GIT_TAG v0.2.0)
FetchContent_MakeAvailable(ym2612_eg)
target_link_libraries(your_target PRIVATE ym2612_eg)
```

Or add `include/` to your include path. `cmake` is only needed for the tests,
the benchmark and the golden-vector generator:

| Variable | Default | Builds |
| -------- | ------- | ------ |
| `YM2612_EG_BUILD_TESTS` | `OFF` | the CTest suite |
| `YM2612_EG_BUILD_BENCH` | `OFF` | the `sample_curve` timing |
| `YM2612_EG_BUILD_GOLDEN_GEN` | `OFF` | the Nuked-OPN2 golden-vector generator |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DYM2612_EG_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## License

MIT.

`tools/golden_gen` fetches and links Nuked-OPN2, which is LGPL 2.1. It is a
development tool, built only behind `YM2612_EG_BUILD_GOLDEN_GEN`, and no part
of it is distributed with the library or linked into the test suite —
`golden/*.json` is data recorded through it.
