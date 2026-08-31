# Golden vectors: where `EgSimulator` and Nuked-OPN2 part company

The vectors in this directory were recorded from **Nuked-OPN2 `ym3438.c`
@ `335747d78cb0abbc3b55b004e62dad9763140115`** by `tools/golden_gen`, and are
replayed against `EgSimulator` by `test/golden_test.cpp`. Over the 110
committed cases the two agree **sample for sample** on `eg_level`, and on
`eg_state` at every EG tick.

Getting there turned up eight places where the two models are not trivially
comparable. Two were bugs in this library and are fixed. Six are genuine
model differences; each one is encoded as a *rule* in `test/golden_common.hpp`
that both the generator and the test evaluate independently, so no vector can
quietly paper over one.

---

## Alignment

Nuked free-runs `eg_timer` from reset; `EgSimulator::reset()` presets the
counter and pins the `/3` divider phase to sample 0. The generator lines them
up like this:

* An EG tick is a sample on which Nuked's `eg_quotient` reaches 2 — that is the
  sample where `eg_timer` is bumped (`OPN2_Clock`, `case 1`) and the only one on
  which `OPN2_EnvelopePrepare` produces a non-zero `eg_inc`.
* Sample 0 of every vector is such a sample, which matches `EgSimulator`
  stepping its counter on its very first `step()`.
* The counter value that drives tick *k* is not `eg_timer` at tick *k*: it is
  the value at tick *k−1*. `eg_shift_lock` and `eg_timer_low_lock` are latched
  at `cycles == 1` of the window *after* the timer bump, and consumed on the
  next update window. So `counter_phase` is recorded as `eg_timer(tick −1)`
  stepped back once, and after that our counter tracks Nuked's latched value
  exactly.
* Key events are placed so that the sample on which `eg_kon` actually changes
  satisfies `sample % 3 == 1` — see "one state per sample" below.

No search or fitting is involved. With that alignment, randomised sweeps of
patches whose rates all stay below 48 match Nuked with **zero** level or state
mismatches.

---

## Documented model differences

### 3. Rates ≥ 48: `eg_timer_low_lock` rotates the increment row by one tick

EG_SPEC 4 flags this. Nuked has no increment table: for `rate >= 48` it uses
`eg_stephi[rate & 3][eg_timer_low_lock] + (rate >> 2) - 11`, and
`eg_timer_low_lock` is a pipeline-latched copy of the timer's low two bits.
The library uses the published 64×8 table, as EG_SPEC 4 recommends.

Driving a single rate for 900 samples and scanning the counter phase over
`{-2, -1, 0, +1}` gives an exact match at exactly one offset, and the offset
depends only on `rate % 4`:

| rate | 44 | 45 | 46 | 47 | 48 | 49 | 50 | 51 | 52 | 53 | 54 | 55 | 56 | 57 | 58 | 59 | 60–63 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| offset that matches Nuked | 0 | 0 | 0 | 0 | any | −1 | −1 | **+1** | any | −1 | −1 | **+1** | any | −1 | −1 | **+1** | any |

"any" is a row whose eight cells are all equal (48, 52, 56, 60–63, and 0/1),
which cannot expose a phase error at all. So:

* `rate < 48` → agrees with the table exactly, offset 0.
* `rate % 4 == 3` → Nuked is one EG tick late.
* otherwise → Nuked is one EG tick early.

The per-four-tick average is identical either way, so this is 56 µs of phase and
nothing else. `golden/high_rate.json` states it exactly rather than loosening
anything: every case there keeps all of its non-constant rows on the same side
of the rotation, records the resulting `counter_shift`, and is then compared
**exactly**. `counter_shift_for_case()` re-derives that number in the test from
the case's own rates and fails the vector if it disagrees.

Cases whose phases would demand different offsets (a mix of rates below and
above 48, or of `rate % 4 == 3` and not) cannot be compared exactly at all; the
generator refuses to emit them. That is why AR 26/28 and the DR=28 column live
in `high_rate.json` with their other rates raised to match, instead of in
`ar_sweep.json` / `dr_sl_grid.json`.

**Limit of the model.** A shifted counter crosses the 12-bit skip-0 wrap
(`0xFFF → 1`) one tick away from the real one, and that wrap is a discontinuity
in `counter & 3`. A shifted case must therefore stay inside one sweep of the
counter (~4095 EG ticks, ~231 ms); `counter_wraps()` enforces it and every
`high_rate` case is 11 000 samples.

### 4. One state transition per sample vs a whole chain per tick

Nuked runs `OPN2_EnvelopeADSR` once per output sample and advances **at most one
state per sample**; this library evaluates the transitions at the top of an EG
tick and can take Attack → Decay → Sustain in one go (EG_SPEC 6: "Both
transitions can fire on the same tick", ymfm's model, the spec's reference
loop).

Normally these agree, because an attack finishes *on* an EG tick and the two
spare samples before the next tick absorb the chain. Two consequences:

* **`eg_state` is compared only on EG-tick samples.** Between ticks Nuked can
  be one or two states ahead of us; by the next tick it never is.
* **SL = 0 together with the instant attack (rate ≥ 62) does not converge.**
  The instant attack sets `att = 0` at the key-on sample, which is *not* a tick,
  so only one spare sample is left. Nuked spends the next tick on the second
  transition and loses that tick's sustain increment, leaving it exactly one EG
  tick (56 µs) behind us for the rest of the note. Reproduce with
  `AR=31 DR=15 SR=31 RR=12 SL=0 KS=3`, C7: with key-on effective at sample 7,
  the first EG tick after it (sample 9) already reads 8 here and 0 in Nuked,
  and the one-step gap never closes.

  Spec and Nuked disagree here (EG_SPEC 6 is explicit that both transitions
  fire on one tick, and cites Shinobi's cymbals), so the library is left alone.
  `has_sl0_instant_attack_divergence()` keeps the combination out of the
  vectors; the SL=0 anchors in `edge_anchors.json` use a real attack instead,
  which does match exactly.

  This is also why key events are placed on `sample % 3 == 1`: a key event on
  the sample immediately before a tick (`% 3 == 2`) leaves Nuked *no* spare
  sample, and one on a tick sample makes Nuked skip the tick's increment
  entirely.

### 5. Decay → Sustain: equality window vs `>=`

Nuked tests `(level >> 4) == (eg_sl[1] << 1)`, a 16-wide window; the library
uses `att >= sustain_att`, as ymfm does and as both specs recommend
(EG_SPEC 6 [DIFF], SSG_EG_SPEC 5.1). A plain decay step is at most 8, so the
window can never be jumped and the two are equivalent. **SSG-EG quadruples the
step**, and 4 × 4 = 16 already clears it: with SSG-EG on, a decay row containing
an increment of 4 or more (rate ≥ 53) and a reachable sustain level
(SL 1..14), the ramp can step straight over the window, after which Nuked stays
in Decay and keeps using DR while we move to SR.

Example: `AR=31 DR=28 SR=26 RR=15 SL=4`, C4, `SSG=$0A`. Decay rate 58, row
`4 8 4 8` × 4 = `16 32 16 32`; the ramp goes 112 → 144 and never lands in
`[128, 143]`.

`has_ssg_sustain_window_divergence()` keeps such cases out. The SSG cases in
`high_rate.json` use SL=15, which sits above the 0x200 freeze so no sustain
transition exists.

### 6. `eg_out` pairs the level with its own inversion flag

The vectors also carry Nuked's `eg_out` (post-inversion, post-TL, clamped) and
compare it against `EgSimulator::output()`. Nuked computes it one pipeline
stage ahead of the level it belongs to — `OPN2_EnvelopeSSGEG` at cycle *slot*,
`OPN2_EnvelopeGenerate` at *slot*+1, `OPN2_EnvelopeADSR` only at *slot*+2 — so:

* `eg_out` lags `eg_level` by exactly one output sample. The generator removes
  that lag before writing the `out` list (`kOutputLagSamples`).
* `eg_out` pairs a level with the inversion flag **that same level produced**,
  while `output()` pairs the new level with the inversion decided on the
  previous one. The two differ only on the single sample where the inversion
  actually flips: at the fold, Nuked emits `0x200 - 0x200 = 0` for one sample
  where we emit `0x200`.

The only thing that can flip the inversion mid-trace is the alternate bit
(bit 1 of `$90`), so `output_comparable()` compares `out` whenever SSG-EG is off
or alternate is clear — which still covers TL, the 0x3FF clamp, and the
permanent inversion of modes 4 and 5. The alternating modes are covered on
`eg_level` and `eg_state` as usual.

### 7. SSG-EG alternate loses a toggle at key-on in Nuked, and never regains it

`OPN2_EnvelopeSSGEG` masks the direction it has just computed with the key
state as it stood *before* the key-on: `direction &= chip->eg_kon[slot]`, and
`eg_kon` is only assigned at the end of `OPN2_EnvelopeADSR`, two cycles later.
So on the first keyed-on sample Nuked's inversion flag is forced clear even
though the level is in the fold region. This library toggles on that sample.

With an instant attack the level is already 0 there, neither model is in the
fold, and the flags stay together — which is why the 28 AR = 31 SSG cases in
`ssg_modes.json`, `retrigger.json`, `edge_anchors.json` and `high_rate.json`
all match. Below rate 62 the level is still 0x3FF on that sample and the two
flags separate by one toggle.

For the modes that only *force* the flag (hold set, `$0B` / `$0F`) the next
sample pulls them back together. For the two that **alternate** it is
permanent: `$0A` and `$0E`, alternate set and hold clear. While the ramp sits
at or above 0x200 both flags flip every sample, so this is just the phase of a
sample-rate square and `output()` still matches Nuked's `eg_out` under its
one-sample lag. The moment the ramp leaves the fold both flags freeze — on
opposite values, for the rest of the note.

Measured with `AR=10 DR=10 SR=6 RR=5 SL=7 TL=0 KS=0`, C4, `SSG=$0A`, key-on
effective at sample 7:

| sample | 7 | 8 | … | 106109 | 106110 | 106111 | … | 113995 |
|---|---|---|---|---|---|---|---|---|
| `eg_level` (both) | 1023 | 1023 | | 512 | 479 | 479 | | 58 |
| our inversion | 1 | 0 | | 1 | **0** | 0 | | **0** |
| Nuked `eg_ssg_inv` | 0 | 1 | | 0 | 1 | **1** | | **1** |
| our `output()` | 513 | 1023 | | 0 | **479** | 479 | | **58** |
| Nuked `eg_out` (lag removed) | 513 | 1023 | | 0 | **33** | 33 | | **454** |

479 against 33 is 42 dB, and it lasts to the end of the note: after the fold
the virtual key-on restarts the ramp, and this library carries its attenuation
down from 479 to 58 while Nuked carries it up from 33 to 454.

`eg_level` itself is untouched — the flag reaches it only through the key-off
latch below — and `output_comparable()` already keeps `out` out of the
alternating modes' comparison. So a `$0A` / `$0E` case with an attack below
rate 62 is still compared on `eg_level` and `eg_state`, for as long as it is
never keyed off — `has_ssg_alternate_keyon_parity_divergence()` rejects the
key-off, and the `$0A` and `$0E` cases in `ssg_slow_attack.json` end while
still keyed on.

### 8. Nuked defers the SSG-EG key-off cut to silence

`key_off()` latches the audible (inverted) level in place of the internal one
and, when SSG-EG leaves that at or above 0x200, cuts straight to 0x3FF on the
key-off sample. Nuked reaches 0x3FF through the same "envelope off" branch as a
plain slot, and that branch is gated on `chip->eg_state[slot]` — the state
*before* the update — not being Attack. A key-off taken from the fold region is
therefore held for one sample with hold set, and two without, because with hold
clear `eg_ssg_repeat_latch` re-asserts `kon_event` on the key-off sample and
pins the state in Attack for it. Nuked emits the latched level meanwhile.

`AR=3 DR=10 SR=6 RR=5 SL=7 TL=0 KS=0`, C4, key-off effective at sample 20002
with the level at 648:

| sample | 20002 | 20003 | 20004 |
|---|---|---|---|
| ours, any mode | 1023 | 1023 | 1023 |
| Nuked `SSG=$08` (hold clear) | 648 | 648 | 1023 |
| Nuked `SSG=$09` (hold set) | 648 | 1023 | 1023 |

The inverting modes latch `0x200 - A` first: `AR=0 SSG=$0C`, C4, level pinned
at 1023, key-off effective at 8002 gives Nuked 513 at 8002 and 8003 and 1023
at 8004, against 1023 throughout here.

The two coincide when the latched level is already 0x3FF, which is where the
hold modes park; `has_ssg_keyoff_cut_divergence()` keeps every other key-off
taken from the fold region out of a vector.

---

## Regenerating

```sh
cmake -B build-gen -DCMAKE_BUILD_TYPE=Release -DYM2612_EG_BUILD_GOLDEN_GEN=ON
cmake --build build-gen --target golden
```

The generator replays every case on `EgSimulator` before writing it and exits
non-zero on the first mismatch, so a committed vector is always one that
reproduced at generation time.
