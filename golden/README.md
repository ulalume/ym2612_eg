# Golden vectors

110 cases recorded from Nuked-OPN2 at the register level: the AR sweep, the
DR x SL grid, the SR/RR sweeps with key-off from every phase, KS across three
octaves, all eight SSG-EG shapes at an instant attack and again under a slow
one, retriggering, edge anchors, and the rate >= 48 regime. Each case stores
Nuked's `eg_level`, `eg_state` and `eg_out` as change lists, plus the
EG-counter phase that lines the two models up; every file documents its own
layout in its `format` field.

`test/golden_test.cpp` replays each case on `EgSimulator` and compares
`eg_level` at every sample, `eg_out` at every sample, and `eg_state` at every
EG tick, with no tolerance. Each scenario file is its own CTest case.

Where the two models genuinely differ — the increment row rotating at
rates >= 48, `SL = 0` with an instant attack, Nuked's 16-wide
`Decay -> Sustain` window under SSG-EG's 4x steps, the SSG-EG inversion flag
losing a toggle at key-on in Nuked, and Nuked deferring the SSG-EG cut to
silence on key-off — the rules live in `test/golden_common.hpp`, evaluated
independently by the generator and the test, and the measurements are in
[`DISCREPANCIES.md`](DISCREPANCIES.md).

## Regenerating

```sh
cmake -B build-gen -DCMAKE_BUILD_TYPE=Release -DYM2612_EG_BUILD_GOLDEN_GEN=ON
cmake --build build-gen --target golden      # rewrites golden/*.json in place
```

The generator replays every case on `EgSimulator` before writing it and exits
non-zero on the first mismatch. Adding or removing a scenario file needs a
`cmake` re-run.

Nuked-OPN2 is LGPL 2.1 and this library is MIT, so it is fetched and linked by
`tools/golden_gen` alone — never by the library, never by the CTest suite.
The committed JSON is data.
