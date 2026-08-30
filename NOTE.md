# Notes

## What is modelled

| Area | Detail |
| ---- | ------ |
| Timing | `clock / 144` output samples, `/3` envelope divider, 12-bit counter that skips 0 on overflow |
| Rates | `ksv = keycode >> (3 - KS)`, `rate = min(2R + ksv, 63)`, `R = 0` holds forever |
| Attack | `att += (~att * inc) >> 4`, instant at rate >= 62, frozen at rates 0/1 and 62/63 |
| Decay / sustain / release | 64x8 increment table, `SL = 15 -> 0x3E0`, transitions at the top of the tick so `SL = 0` skips decay |
| SSG-EG | Per-sample state machine: 4x increments, freeze at `0x200`, output inversion, virtual key-on, key-off latch and hard cut, all 8 shapes |
| Durations | Closed-form phase lengths and SSG loop period, `infinity` where a rate never advances |
| Output | `min((J ? 0x200 - A : A) + TL * 8, 0x3FF)` |


## Why the durations are closed forms

A caller sizing a time axis needs to know how long a phase lasts *before* it
knows how far to simulate. Answering that by running the simulator and looking
makes "how far did you look?" part of the answer, and that is a place where an
answer changes discontinuously: a rate of 0 really does hold forever, and a run
that gave up after three seconds cannot tell `SR = 0` from `SR = 31`. So
`phase_durations()` and `ssg_loop_period_ms()` compute rather than observe, and
report `infinity` for a phase that never advances.

The post-attack phases are linear in attenuation, so each is one division. The
attack recurrence and the SSG ramp are walked instead — a few hundred table
slots, against the hundreds of thousands of samples the same stretch costs to
simulate.

## How closely the closed forms match the simulator

`test/timing_test.cpp` cross-checks both against `sample_curve()`:

| Check | Patches | Worst |
| ----- | ------- | ----- |
| Decay end | 1260 | 1.99% |
| Lifetime vs `park_ms` | 1010 | 1.99% |
| SSG loop period | 3944 | 3.155% (0.681% at `AR = 31`) |

The residue is not model error. The EG's 12-bit counter is free-running and
shared, so the chip's own ramps vary from one to the next — at `AR = 14` they
measure 201.8 / 194.6 / 187.4 / 194.6 ms — and the two answers average a
different number of them.

## The weak corner: SL = 15

`SL = 15` is `0x3E0` and the cut is `0x3F0`, so the sustain phase is sixteen
attenuation units wide. A decay stops at the first increment *past* the sustain
level, so a single step of overshoot can be most of the phase: with a slow `SR`
the closed-form lifetime is out by up to 45%. Every other `SL` stays under
3.6%.

This is a property of the chip's own arithmetic, not of the approximation, and
it is pinned by `test_a_full_sustain_level_leaves_a_sixteen_unit_tail`. A
caller that compresses the lifetime (as a time axis usually must) sees roughly
half of it.

## Cost

`sample_curve()` costs about 0.4 ms per second of simulated span, whatever the
patch: a 10 s span is 3–5 ms. It steps every output sample because the SSG-EG
state machine has to, and a plain ADSR pays the same price for five vertices.
Skipping the linear stretches would need exactly the closed forms
`phase_durations()` already computes.

## Golden vectors

Where this library and Nuked-OPN2 part company, and why, is in
[`golden/DISCREPANCIES.md`](golden/DISCREPANCIES.md).
