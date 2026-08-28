#pragma once

// Sample-accurate YM2612 (OPN2) envelope generator simulator, one operator.
//
// Normative references:
//   EG_SPEC.md      - base envelope generator (timing, rates, ADSR)
//   SSG_EG_SPEC.md  - SSG-EG state machine
//
// This is a *visualisation* model: no phase generator, no sine/exp tables,
// no channel mixing.  Everything that changes the envelope shape is modelled
// exactly, including the 12-bit skip-0 counter and the /3 EG divider.

#include "constants.hpp"
#include "tables.hpp"

#include <cstdint>

namespace ym2612_eg {

struct CurveRequest;
struct CurveResult;
inline CurveResult sample_curve(const CurveRequest &request);

// Per-operator register values, already unpacked.
struct OperatorParams {
  uint8_t ar = 0;  // attack rate, 0..31        ($50-$5F bits 0-4)
  uint8_t dr = 0;  // first decay rate, 0..31   ($60-$6F bits 0-4)
  uint8_t sr = 0;  // second decay rate, 0..31  ($70-$7F bits 0-4)
  uint8_t rr = 0;  // release rate, 0..15       ($80-$8F bits 0-3)
  uint8_t sl = 0;  // sustain level, 0..15      ($80-$8F bits 4-7)
  uint8_t tl = 0;  // total level, 0..127       ($40-$4F bits 0-6)
  uint8_t ks = 0;  // key scale / RS, 0..3      ($50-$5F bits 6-7)
  uint8_t ssg = 0; // SSG-EG, 4 bits            ($90-$9F bits 0-3)
                   //   bit3 enable, bit2 attack, bit1 alternate, bit0 hold
};

// Pitch matters: key scaling derives from block + F-num (EG_SPEC 3, 8).
struct NotePitch {
  uint16_t fnum = 0; // 11-bit
  uint8_t block = 0; // 0..7

  // Mirrors megatoy's note -> (fnum, block) mapping exactly:
  // ym2612::Note::from_midi_note() followed by frequency_with_bend(note, 0).
  static NotePitch from_midi(int midi_note);

  // EG_SPEC 3: keycode = (block << 2) | (F11 << 1) | lsb.
  uint8_t keycode() const;
};

enum class EgPhase : uint8_t { Attack, Decay, Sustain, Release };

namespace detail {

// megatoy src/ym2612/note.hpp, fnote_from_key(): C .. B.
inline constexpr uint16_t kMegatoyFnum[12] = {322, 341, 361, 383, 406, 430,
                                              455, 482, 511, 541, 574, 608};

// Events raised by a single EgSimulator::step(); consumed by sample_curve().
enum EventBits : uint32_t {
  kEvAttackEnd = 1u << 0,
  kEvDecayEnd = 1u << 1,
  kEvSsgFold = 1u << 2,
  kEvSsgInvert = 1u << 3,
  kEvSsgHold = 1u << 4,
  kEvSsgPhaseReset = 1u << 5,
};

} // namespace detail

inline NotePitch NotePitch::from_midi(int midi_note) {
  const int m = midi_note < 0 ? 0 : (midi_note > 127 ? 127 : midi_note);
  // megatoy computes `int octave = midi_note / 12 - 1` in signed arithmetic
  // and clamps negative octaves up to 0.  MIDI 0..11 (octave -1) therefore
  // lands on block 0 -- the lowest representable octave -- rather than
  // wrapping to the highest.  Replicated here so the graph matches what
  // megatoy actually plays.
  int octave = m / 12 - 1;
  if (octave < 0) {
    octave = 0;
  }
  const uint8_t block =
      octave > 7 ? uint8_t{7} : static_cast<uint8_t>(octave);
  // frequency_with_bend() renormalises F-num into [322, 644); every table
  // entry is already inside that window, so block is untouched.
  return NotePitch{detail::kMegatoyFnum[m % 12], block};
}

inline uint8_t NotePitch::keycode() const {
  const unsigned f = fnum & 0x7FFu;
  const unsigned f11 = (f >> 10) & 1u;
  // lsb = (F11 & (F10|F9|F8)) | (~F11 & F10 & F9 & F8), in ymfm's LUT form.
  const unsigned lsb = (0xFE80u >> ((f >> 7) & 0x0Fu)) & 1u;
  return static_cast<uint8_t>(((block & 7u) << 2) | (f11 << 1) | lsb);
}

class EgSimulator {
public:
  EgSimulator(const OperatorParams &params, NotePitch pitch,
              double clock_hz = kNtscClockHz)
      : params_(params), pitch_(pitch), clock_hz_(clock_hz) {
    recompute();
    reset(0);
  }

  // Mid-note register write: rates are recomputed immediately (EG_SPEC 3).
  void set_params(const OperatorParams &params) {
    params_ = params;
    recompute();
  }

  void set_pitch(NotePitch pitch) {
    pitch_ = pitch;
    recompute();
  }

  // Edge-triggered, exactly like a $28 write (EG_SPEC 7).
  void key_on() {
    if (keyed_on_)
      return;
    keyed_on_ = true;
    ssg_invert_ = false; // SSG_EG_SPEC 2c: inversion flag cleared on key-on
    ssg_held_ = false;
    phase_ = EgPhase::Attack;
    // Attenuation is NOT reset; attack resumes from the current level,
    // except the instant-attack case.
    if (rate_[0] >= 62)
      att_ = 0;
  }

  void key_off() {
    if (!keyed_on_)
      return;
    // SSG_EG_SPEC 3: the *audible* (inverted) level is latched in place, so
    // release continues from what was heard, not from the internal level.
    if (ssg_enable_ && (ssg_attack_ != ssg_invert_))
      att_ = (0x200 - att_) & 0x3FF;
    ssg_invert_ = false;
    keyed_on_ = false;
    phase_ = EgPhase::Release;
  }

  // Advance one output sample (clock / 144).  SSG-EG logic runs every sample
  // and *before* the envelope update; the envelope itself advances once per
  // three samples (SSG_EG_SPEC 2b).
  void step() {
    events_ = 0;
    if (ssg_enable_)
      ssg_step();
    if (eg_divider_ == 0)
      eg_step();
    if (++eg_divider_ == kEgClockDivider)
      eg_divider_ = 0;
    ++samples_;
  }

  void step(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
      step();
  }

  // Re-initialise.  counter_phase presets the free-running 12-bit EG counter
  // (it is shared by all 24 operators on real hardware and is not reset by
  // key-on, so the phase at key-on jitters the first update by up to one
  // period).  start_att presets the attenuation, which is what a retrigger
  // into a still-sounding operator looks like.
  void reset(uint16_t counter_phase = 0, uint16_t start_att = kMaxAttenuation) {
    counter_ = counter_phase & 0x0FFF;
    att_ = start_att > kMaxAttenuation ? kMaxAttenuation : start_att;
    phase_ = EgPhase::Release;
    keyed_on_ = false;
    ssg_invert_ = false;
    ssg_held_ = false;
    eg_divider_ = 0;
    samples_ = 0;
    events_ = 0;
  }

  uint16_t attenuation() const { return static_cast<uint16_t>(att_); }

  // Inversion (keyed-on only) then TL, clamped.  EG_SPEC 2, SSG_EG_SPEC 5.
  uint16_t output() const {
    int a = att_;
    if (ssg_enable_ && keyed_on_ && (ssg_attack_ != ssg_invert_))
      a = (0x200 - a) & 0x3FF;
    const int o = a + (static_cast<int>(params_.tl & 0x7F) << 3);
    return static_cast<uint16_t>(o > kMaxAttenuation ? kMaxAttenuation : o);
  }

  EgPhase phase() const { return phase_; }
  bool keyed_on() const { return keyed_on_; }

  // Effective inversion J = attack_bit XOR invert_flag.
  bool ssg_inverted() const {
    return ssg_enable_ && (ssg_attack_ != ssg_invert_);
  }

  // True when nothing can change without an external event (a register write
  // or a key on/off).  Used to cut simulation short.
  bool is_static() const {
    if (phase_ == EgPhase::Attack) {
      if (att_ == 0)
        return false; // -> Decay on the next EG tick
      if (ssg_churning())
        return false;
      // Rate 0/1 (only reachable with AR=0) and rates 62/63 both freeze the
      // attack: rows 0/1 of the table are all-zero, and the update itself is
      // guarded by `rate < 62` (EG_SPEC 5).
      return rate_[0] < 2 || rate_[0] >= 62;
    }
    if (ssg_enable_ && att_ >= kSsgFoldAttenuation) {
      // Keyed off: the hard cut to 0x3FF happens on the next sample, and once
      // it has happened nothing else can move.
      if (!keyed_on_)
        return att_ >= kMaxAttenuation;
      // Hold clear -> virtual key-on restarts the ramp every cycle.
      // Hold set   -> static only once the mode has actually latched; the
      // sample that first lands on 0x200 still has the latch ahead of it.
      return ssg_hold_ && ssg_held_;
    }
    if (att_ >= kMaxAttenuation)
      return true;
    // A Decay that already satisfies the sustain test moves to Sustain on the
    // next tick, so judge it by the sustain rate.
    int idx = static_cast<int>(phase_);
    if (phase_ == EgPhase::Decay && att_ >= sustain_att_)
      idx = static_cast<int>(EgPhase::Sustain);
    return rate_[idx] == 0;
  }

  double time_ms() const {
    return static_cast<double>(samples_) * 1000.0 / sample_rate_hz(clock_hz_);
  }

  // Introspection used by the tests and by sample_curve().
  int rate_of(EgPhase p) const { return rate_[static_cast<int>(p)]; }
  int sustain_attenuation() const { return sustain_att_; }
  int key_scale_value() const { return ksv_; }
  double clock_hz() const { return clock_hz_; }

private:
  friend CurveResult sample_curve(const CurveRequest &request);

  uint32_t step_events() const { return events_; }

  void recompute() {
    ksv_ = pitch_.keycode() >> (3 - (params_.ks & 3));
    rate_[0] = detail::effective_rate(params_.ar & 0x1F, ksv_);
    rate_[1] = detail::effective_rate(params_.dr & 0x1F, ksv_);
    rate_[2] = detail::effective_rate(params_.sr & 0x1F, ksv_);
    // Release register is 4-bit: R = 2*RR + 1, so it can never be 0.
    rate_[3] = detail::effective_rate(2 * (params_.rr & 0x0F) + 1, ksv_);
    sustain_att_ = detail::sustain_attenuation(params_.sl);
    ssg_enable_ = (params_.ssg & 0x08) != 0;
    ssg_attack_ = (params_.ssg & 0x04) != 0;
    ssg_alternate_ = (params_.ssg & 0x02) != 0;
    ssg_hold_ = (params_.ssg & 0x01) != 0;
  }

  // Would the SSG block keep firing (and therefore keep changing something)?
  bool ssg_churning() const {
    return ssg_enable_ && att_ >= kSsgFoldAttenuation && !ssg_hold_;
  }

  // SSG_EG_SPEC 2b.  Runs once per output sample, gated on A >= 0x200.
  // Step 1 must precede step 4; the rest are order-independent.
  void ssg_step() {
    if (att_ < kSsgFoldAttenuation)
      return;

    // 1. alternate -> toggle inversion; alternate+hold -> force it set.
    if (ssg_alternate_) {
      const bool before = ssg_invert_;
      ssg_invert_ = ssg_hold_ ? true : !ssg_invert_;
      if (ssg_invert_ != before)
        events_ |= detail::kEvSsgInvert;
    }

    // 2. neither alternate nor hold -> the phase generator is forced to 0.
    //    We do not model the PG; the moment is reported so the UI can mark it.
    if (!ssg_alternate_ && !ssg_hold_)
      events_ |= detail::kEvSsgPhaseReset;

    // 3. keyed on and hold clear -> virtual key-on (this is the 0x200 -> 0 snap).
    if (keyed_on_ && !ssg_hold_) {
      phase_ = EgPhase::Attack;
      if (rate_[0] >= 62)
        att_ = 0;
      events_ |= detail::kEvSsgFold;
    }

    // 4. hold set -> the mode latches here, once.  When the output is *not*
    //    inverted the level jumps to silence; the two inverted-hold modes
    //    (3 and 5) instead freeze at 0x200, i.e. full output volume.
    if (ssg_hold_ && phase_ != EgPhase::Attack) {
      if (!ssg_held_) {
        ssg_held_ = true;
        events_ |= detail::kEvSsgHold;
      }
      if (!(ssg_attack_ != ssg_invert_))
        att_ = kMaxAttenuation;
    }

    // 5. keyed off -> hard cut, unconditionally (SSG_EG_SPEC 3.4).
    if (!keyed_on_)
      att_ = kMaxAttenuation;
  }

  // One EG tick, clock / 432.  EG_SPEC "reference tick loop".
  void eg_step() {
    // 12-bit free-running counter that skips 0 on overflow.
    counter_ = (counter_ + 1) & 0x0FFF;
    if (counter_ == 0)
      counter_ = 1;

    // Transitions are checked at the top of the tick, before any increment,
    // so both can fire on the same tick and SL=0 skips decay entirely.
    if (phase_ == EgPhase::Attack && att_ == 0) {
      phase_ = EgPhase::Decay;
      events_ |= detail::kEvAttackEnd;
    }
    if (phase_ == EgPhase::Decay && att_ >= sustain_att_) {
      phase_ = EgPhase::Sustain;
      events_ |= detail::kEvDecayEnd;
    }

    const int rate = rate_[static_cast<int>(phase_)];
    const int shift = detail::rate_shift(rate);
    if (counter_ & ((1 << shift) - 1))
      return;
    const int inc = detail::kIncTable[rate][(counter_ >> shift) & 7];

    if (phase_ == EgPhase::Attack) {
      // Exponential, and the shift must be arithmetic: ~att is negative.
      static_assert((-16 >> 4) == -1, "arithmetic right shift required");
      if (rate < 62 && inc != 0)
        att_ += (~att_ * inc) >> 4;
      return;
    }

    if (ssg_enable_) {
      // SSG_EG_SPEC 2a: 4x increment, and a hard freeze at 0x200.
      // Applies to DR, SR and RR alike.
      if (att_ < kSsgFoldAttenuation)
        att_ += 4 * inc;
    } else {
      att_ += inc;
    }
    if (att_ > kMaxAttenuation)
      att_ = kMaxAttenuation;

    // Nuked behavior: in any non-attack phase, once (att & 0x3F0) == 0x3F0 the
    // hardware forces att = 0x3FF and state = Release in one step.  ymfm just
    // clamps; we follow Nuked because the golden vectors come from Nuked-OPN2.
    if ((att_ & 0x3F0) == 0x3F0) {
      att_ = kMaxAttenuation;
      phase_ = EgPhase::Release;
    }
  }

  OperatorParams params_{};
  NotePitch pitch_{};
  double clock_hz_ = kNtscClockHz;

  int rate_[4] = {0, 0, 0, 0}; // indexed by EgPhase
  int sustain_att_ = 0;
  int ksv_ = 0;
  bool ssg_enable_ = false;
  bool ssg_attack_ = false;
  bool ssg_alternate_ = false;
  bool ssg_hold_ = false;

  int att_ = kMaxAttenuation;
  int counter_ = 0;
  int eg_divider_ = 0;
  EgPhase phase_ = EgPhase::Release;
  bool keyed_on_ = false;
  bool ssg_invert_ = false;
  bool ssg_held_ = false;
  uint64_t samples_ = 0;
  uint32_t events_ = 0;
};

} // namespace ym2612_eg
