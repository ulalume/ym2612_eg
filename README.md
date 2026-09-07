# ym2612_eg

A sample-accurate YM2612 / OPN2 envelope generator simulator for visualisation.
Header-only C++17, no dependencies.

Not an audio emulator: no phase generator, no sine/exp tables, no channel
mixing. Everything that changes the shape of an operator's envelope is
modelled at the chip's own sample rate.

Every register that shapes an operator's envelope is modelled, down to the
free-running counter the rates are quantised against ([NOTE.md](NOTE.md)). LFO
AM, the phase generator and algorithm routing are out of scope; draw them as
separate overlays if you need them.

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

## Durations

```cpp
const PhaseDurations d = phase_durations(req.op, req.pitch);
d.attack_ms; d.decay_ms; d.sustain_ms;   // sustain level -> silence
d.sustain_start_ms();                    // attack + decay
d.lifetime_ms();                         // key-on -> silence

ssg_loop_period_ms(req.op, req.pitch);   // 0 = the mode does not loop
                                         // inf = a rate the ramp needs stalls

key_scale_value(op, pitch);   // keycode >> (3 - KS): the only route a note
                              // takes into an envelope, so two notes sharing
                              // it have bit-identical curves
loudest_attenuation(op);      // 0, or 0x200 for the inverted SSG-EG modes
sustain_attenuation(sl);      // SL -> attenuation; SL = 15 is 0x3E0
```

Computed rather than simulated, so `infinity` means a phase never advances.
Both take an optional `clock_hz`. Their accuracy against the simulator is
measured in [NOTE.md](NOTE.md).

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

## Graph

`ym2612_eg::graph` is the policy a drawn envelope needs on top of all that:
how wide the time axis should be, the two traces to put on it, and where a
sounding voice sits on them.

```cpp
using namespace ym2612_eg::graph;

const EnvelopeCurve c = build_envelope_curve(req.op, req.pitch);
c.span_ms;         // the time axis, in ms; both traces are simulated across it
c.held;            // attack, decay and sustain, key never released
c.release;         // a release from full volume, starting at t = 0
c.attack_end_ms;   // ... and c.decay_end_ms, c.ssg_folds: where the line breaks
c.peak_out;        // ... and c.sustain_out: levels to draw guides at
c.warning;         // one line, or nullptr

grid_step_ms(c.span_ms);   // a round label interval, 3-6 divisions

// One voice: ms since key-on, and since key-off (negative while it is held).
const VoiceCursor v = cursor_for_voice(c, 120.0, -1.0, c.span_ms);
v.ms;               // where on the axis it is
v.held_to_ms;       // how much of the held trace it has been through
v.release_from_ms;  // where its release joins the drawn one
v.silent_for_ms;    // 0 until it is inaudible; fade it out on this
```

The two traces share the axis and nothing else. Chaining them would mean
inventing a key-off instant, which cuts the sustain at an arbitrary level, so
the held trace is simulated with the key never released — the only way `SR`
reads truthfully — and the release runs on its own from full volume.

The axis is the envelope's own length ([NOTE.md](NOTE.md)), so every rate moves
it and moves it one way. Pass `min_span_ms` to draw a second note's curve on
the first one's axis rather than extrapolating it past its own window.

`EnvelopeCurveCache` holds one operator's curve and rebuilds it when a register
that shapes it changes, spacing expensive rebuilds by what the last one cost
(`RebuildThrottle`, whose clock a test can replace). `VoiceCurveCache` keys the
sounding notes on `key_scale_value()` instead of on the note, so a chord inside
one octave shares a single curve and most note-ons simulate nothing; it builds
at most `build_budget` curves per call and defers the rest.

## CMake

```cmake
include(FetchContent)
FetchContent_Declare(ym2612_eg
  GIT_REPOSITORY https://github.com/ulalume/ym2612_eg.git
  GIT_TAG v0.2.2)
FetchContent_MakeAvailable(ym2612_eg)
target_link_libraries(your_target PRIVATE ym2612_eg)
```

Or add `include/` to your include path. `cmake` is only needed for the tests,
the benchmark and the golden-vector generator:

| Variable                     | Default | Builds                                 |
| ---------------------------- | ------- | -------------------------------------- |
| `YM2612_EG_BUILD_TESTS`      | `OFF`   | the CTest suite                        |
| `YM2612_EG_BUILD_BENCH`      | `OFF`   | the `sample_curve` timing              |
| `YM2612_EG_BUILD_GOLDEN_GEN` | `OFF`   | the Nuked-OPN2 golden-vector generator |

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
