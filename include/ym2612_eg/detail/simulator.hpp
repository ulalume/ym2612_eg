#pragma once

// YM2612 (OPN2) envelope generator, one operator, stepped at the chip's
// sample rate.  No phase generator, no sine/exp tables, no channel mixing.

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

// Key scaling derives from block + F-num, so pitch changes the envelope.
struct NotePitch {
  uint16_t fnum = 0; // 11-bit
  uint8_t block = 0; // 0..7

  // F-num from the note table, block = midi/12 - 1 clamped to 0..7:
  // MIDI 60 is middle C at block 4, F-num 644.
  static NotePitch from_midi(int midi_note);

  // keycode = (block << 2) | (F11 << 1) | lsb.
  uint8_t keycode() const;
};

enum class EgPhase : uint8_t { Attack, Decay, Sustain, Release };

namespace detail {

// F-number of each note, C .. B.
inline constexpr uint16_t kNoteFnum[12] = {644, 682, 723, 766, 811, 859,
                                           910, 965, 1022, 1083, 1147, 1215};

// What EgSimulator::skippable_samples() reports when the envelope can only be
// moved again by an external event.  Far past any axis a caller can ask for,
// and small enough to keep the sample arithmetic inside 32 bits.
inline constexpr uint32_t kUnboundedSkip = 1u << 30;

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
  // octave = midi/12 - 1, negative octaves clamped up, so MIDI 0..11 land on
  // block 0.
  int octave = m / 12 - 1;
  if (octave < 0) {
    octave = 0;
  }
  const uint8_t block =
      octave > 7 ? uint8_t{7} : static_cast<uint8_t>(octave);
  // Every table entry lies in [644, 1288), so block is the octave.
  return NotePitch{detail::kNoteFnum[m % 12], block};
}

inline uint8_t NotePitch::keycode() const {
  const unsigned f = fnum & 0x7FFu;
  const unsigned f11 = (f >> 10) & 1u;
  // lsb = (F11 & (F10|F9|F8)) | (~F11 & F10 & F9 & F8), in ymfm's LUT form.
  const unsigned lsb = (0xFE80u >> ((f >> 7) & 0x0Fu)) & 1u;
  return static_cast<uint8_t>(((block & 7u) << 2) | (f11 << 1) | lsb);
}

// The key scale value, `keycode >> (3 - KS)`: the ONLY route a note takes into
// an envelope, and so the whole of what a pitch is worth to a caller that
// caches curves.  Two notes that share it have bit-identical envelopes.
inline int key_scale_value(const OperatorParams &op, NotePitch pitch) {
  return pitch.keycode() >> (3 - (op.ks & 3));
}

// The internal attenuation at which this operator is at its LOUDEST, i.e. the
// level a key-on starts a release from. Normally 0, the top of the scale.
// With SSG-EG enabled and the attack bit set (types 4-7) output() inverts,
// `(0x200 - A) & 0x3FF`, so the scale runs the other way: 0 is the quietest
// point the ramp reaches and 0x200 is full volume. On key-on the inversion
// flag is clear, so the attack bit alone decides.
inline uint16_t loudest_attenuation(const OperatorParams &op) {
  const bool inverted = (op.ssg & 0x08) != 0 && (op.ssg & 0x04) != 0;
  return inverted ? kSsgFoldAttenuation : uint16_t{0};
}

class EgSimulator {
public:
  EgSimulator(const OperatorParams &params, NotePitch pitch,
              double clock_hz = kNtscClockHz)
      : params_(params), pitch_(pitch), clock_hz_(clock_hz) {
    recompute();
    reset(0);
  }

  // Mid-note register write: rates are recomputed immediately.
  void set_params(const OperatorParams &params) {
    params_ = params;
    recompute();
  }

  void set_pitch(NotePitch pitch) {
    pitch_ = pitch;
    recompute();
  }

  // Edge-triggered, like a $28 write.  A write takes one output sample to
  // reach the envelope: until the next step() the SSG-EG block still acts on
  // the key state it had before.
  void key_on() {
    if (keyed_on_)
      return;
    keyed_on_ = true;
    ssg_held_ = false;
    ssg_in_fold_ = false;
    phase_ = EgPhase::Attack;
    // Attenuation is NOT reset; attack resumes from the current level,
    // except the instant-attack case.
    if (rate_[0] >= 62)
      att_ = 0;
  }

  void key_off() {
    if (!keyed_on_)
      return;
    keyed_on_ = false;
    phase_ = EgPhase::Release;
  }

  // Advance one output sample (clock / 144).  SSG-EG logic runs every sample
  // and *before* the envelope update; the envelope itself advances once per
  // three samples.
  void step() {
    events_ = 0;
    if (ssg_enable_)
      ssg_step();
    else
      envelope_off_step();
    if (eg_divider_ == 0)
      eg_step();
    // The key state and the phase are both latched at the end of the update,
    // so the next sample acts on what this one started with.
    keyed_on_at_start_ = keyed_on_;
    phase_at_start_ = phase_;
    if (++eg_divider_ == kEgClockDivider)
      eg_divider_ = 0;
    ++samples_;
  }

  void step(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
      step();
  }

  // How many output samples may pass with nothing observable happening: no
  // event, and no move in attenuation, phase or SSG state.  0 when the very
  // next sample can already change something; detail::kUnboundedSkip when
  // only an external event can.
  uint32_t skippable_samples() const {
    // A key write has not reached the envelope yet, and the sample that lets
    // it through is not like the ones around it.
    if (keyed_on_ != keyed_on_at_start_)
      return 0;
    if (ssg_enable_) {
      // The key state masks the direction flag out on the sample after a
      // key-off, whatever the level is doing.
      if (ssg_invert_ && !keyed_on_at_start_)
        return 0;
      if (att_ >= kSsgFoldAttenuation && !ssg_fold_idle())
        return 0;
    } else if (phase_ != EgPhase::Attack && (att_ & 0x3F0) == 0x3F0 &&
               att_ != kMaxAttenuation) {
      // envelope_off_step() snaps to silence on the next output sample.
      return 0;
    }

    return samples_to_eg_move();
  }

  // The SSG-EG alternate fold flips the inversion flag once per output sample,
  // so while attenuation stands still the output squares between two levels
  // at the sample rate -- a band, not a line. Returns how many samples that
  // lasts, 0 when not in it; `first`/`second` are the next two output levels,
  // which then repeat.
  uint32_t alternating_samples(uint16_t &first, uint16_t &second) const {
    if (!ssg_enable_ || !ssg_alternate_ || ssg_hold_ || !keyed_on_ ||
        !keyed_on_at_start_ || att_ < kSsgFoldAttenuation)
      return 0;
    // Entering the fold raises events, and the virtual key-on has to have
    // already put the phase where it then keeps it.  At rate >= 62 that same
    // branch zeroes the level instead of leaving it folded.
    if (!ssg_in_fold_ || phase_ != EgPhase::Attack || rate_[0] >= 62)
      return 0;
    const uint32_t n = samples_to_eg_move();
    if (n == 0)
      return 0;
    first = output_with(ssg_attack_ == ssg_invert_);
    second = output_with(ssg_attack_ != ssg_invert_);
    return n;
  }

  // Advance `samples` output samples at once.  Defined only for a count at or
  // below skippable_samples() or alternating_samples(); past that the states
  // in between are not all alike and have to be walked.
  void skip(uint32_t samples) {
    if (samples == 0)
      return;
    events_ = 0;
    // ssg_step() clears the fold latch on every sample spent below the fold,
    // and above it flips the inversion flag once per sample unless hold pins
    // the flag set.
    if (ssg_enable_ && att_ < kSsgFoldAttenuation)
      ssg_in_fold_ = false;
    else if (ssg_enable_ && ssg_alternate_ && !ssg_hold_ && keyed_on_at_start_)
      ssg_invert_ = ssg_invert_ != ((samples & 1u) != 0);

    const uint32_t to_tick =
        static_cast<uint32_t>((kEgClockDivider - eg_divider_) %
                              kEgClockDivider);
    if (samples > to_tick) {
      const uint64_t ticks =
          (samples - to_tick + kEgClockDivider - 1) / kEgClockDivider;
      // The counter skips 0, so it walks 1..4095 with a period of 4095.
      const uint64_t base = static_cast<uint64_t>((counter_ + 4094) % 4095);
      counter_ = static_cast<int>((base + ticks) % 4095) + 1;
    }
    eg_divider_ =
        static_cast<int>((static_cast<uint32_t>(eg_divider_) + samples) %
                         kEgClockDivider);
    samples_ += samples;
  }

  // counter_phase presets the 12-bit EG counter, which is shared by all 24
  // operators and not reset by key-on, so its phase jitters the first update
  // by up to one period.  start_att presets the attenuation for a retrigger.
  void reset(uint16_t counter_phase = 0, uint16_t start_att = kMaxAttenuation) {
    counter_ = counter_phase & 0x0FFF;
    att_ = start_att > kMaxAttenuation ? kMaxAttenuation : start_att;
    phase_ = EgPhase::Release;
    phase_at_start_ = EgPhase::Release;
    keyed_on_ = false;
    keyed_on_at_start_ = false;
    ssg_invert_ = false;
    ssg_held_ = false;
    ssg_in_fold_ = false;
    eg_divider_ = 0;
    samples_ = 0;
    events_ = 0;
  }

  uint16_t attenuation() const { return static_cast<uint16_t>(att_); }

  // Inversion (only once a key-on has reached the envelope) then TL, clamped.
  uint16_t output() const {
    return output_with(ssg_enable_ && keyed_on_at_start_ &&
                       (ssg_attack_ != ssg_invert_));
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
    // A key write still has a sample to travel before the envelope sees it,
    // and the direction flag it leaves behind is masked out a sample later.
    if (keyed_on_ != keyed_on_at_start_ || (ssg_enable_ && ssg_invert_ && !keyed_on_at_start_))
      return false;
    if (phase_ == EgPhase::Attack) {
      if (att_ == 0)
        return false; // -> Decay on the next EG tick
      if (ssg_churning())
        return false;
      // Rate 0/1 (only reachable with AR=0) and rates 62/63 both freeze the
      // attack: rows 0/1 of the table are all-zero, and the update is guarded
      // by `rate < 62`.
      return rate_[0] < 2 || rate_[0] >= 62;
    }
    if (ssg_enable_ && att_ >= kSsgFoldAttenuation) {
      // Keyed off: the cut to 0x3FF still has the walk out of Attack in front
      // of it, and once both are behind nothing else can move.
      if (!keyed_on_)
        return att_ >= kMaxAttenuation && phase_ == EgPhase::Release;
      // Hold clear -> virtual key-on restarts the ramp every cycle.
      // Hold set   -> static only once the mode has actually latched; the
      // sample that first lands on 0x200 still has the latch ahead of it.
      return ssg_hold_ && ssg_held_;
    }
    if (att_ >= kMaxAttenuation)
      return true;
    // The snap to 0x3FF is still pending on the next output sample, even if
    // the phase rate is 0 (reachable by writing SR = 0 mid-decay).
    if (!ssg_enable_ && (att_ & 0x3F0) == 0x3F0)
      return false;
    // A Decay that already satisfies the sustain test still has that
    // transition ahead of it: reporting rest here would end a caller's run
    // one tick early and swallow the Decay -> Sustain event.
    if (phase_ == EgPhase::Decay && att_ >= sustain_att_)
      return false;
    return rate_[static_cast<int>(phase_)] == 0;
  }

  double time_ms() const {
    return static_cast<double>(samples_) * 1000.0 / sample_rate_hz(clock_hz_);
  }

  // Effective values after key scaling.
  int rate_of(EgPhase p) const { return rate_[static_cast<int>(p)]; }
  int sustain_attenuation() const { return sustain_att_; }
  int key_scale_value() const { return ksv_; }
  double clock_hz() const { return clock_hz_; }

private:
  friend CurveResult sample_curve(const CurveRequest &request);

  uint32_t step_events() const { return events_; }

  void recompute() {
    // Qualified: the member below shadows the free function's name.
    ksv_ = ym2612_eg::key_scale_value(params_, pitch_);
    rate_[0] = detail::effective_rate(params_.ar & 0x1F, ksv_);
    rate_[1] = detail::effective_rate(params_.dr & 0x1F, ksv_);
    rate_[2] = detail::effective_rate(params_.sr & 0x1F, ksv_);
    // Release register is 4-bit: R = 2*RR + 1, so it can never be 0.
    rate_[3] = detail::effective_rate(2 * (params_.rr & 0x0F) + 1, ksv_);
    sustain_att_ = ym2612_eg::sustain_attenuation(params_.sl);
    ssg_enable_ = (params_.ssg & 0x08) != 0;
    ssg_attack_ = (params_.ssg & 0x04) != 0;
    ssg_alternate_ = (params_.ssg & 0x02) != 0;
    ssg_hold_ = (params_.ssg & 0x01) != 0;
  }

  // The SSG block keeps firing, so something keeps changing.
  bool ssg_churning() const {
    return ssg_enable_ && att_ >= kSsgFoldAttenuation && !ssg_hold_;
  }

  // True when ssg_step() at or above the fold level has nothing left to do:
  // every latch already set, every jump already taken, every event already
  // raised.  Only meaningful with SSG-EG on and att_ >= kSsgFoldAttenuation.
  bool ssg_fold_idle() const {
    // The fold is entered once, and the entry is what raises the events.
    if (!ssg_in_fold_)
      return false;
    // Alternate flips the inversion flag on every single sample, unless hold
    // pins it set and it has already got there.
    if (ssg_alternate_ && keyed_on_at_start_ && !(ssg_hold_ && ssg_invert_))
      return false;
    // The virtual key-on re-enters attack, and at rate >= 62 zeroes the level.
    if (keyed_on_ && !ssg_hold_ &&
        (phase_ != EgPhase::Attack || rate_[0] >= 62))
      return false;
    // Hold: the mode latch, and for the non-inverted modes the jump to
    // silence, are both already behind us.
    if (ssg_hold_ && phase_ != EgPhase::Attack) {
      if (!ssg_held_)
        return false;
      if (ssg_attack_ == ssg_invert_ &&
          (att_ != kMaxAttenuation || phase_ != EgPhase::Release))
        return false;
    }
    // Keyed off, the hard cut runs unconditionally.
    if (!keyed_on_ && (att_ != kMaxAttenuation || phase_ != EgPhase::Release))
      return false;
    return true;
  }

  // Output samples until an EG tick can move the attenuation or the phase;
  // detail::kUnboundedSkip when none ever will.
  uint32_t samples_to_eg_move() const {
    // An EG tick lands on the samples whose divider reads 0 on entry.
    const uint32_t to_tick =
        static_cast<uint32_t>((kEgClockDivider - eg_divider_) %
                              kEgClockDivider);

    // A transition test that already holds fires on that tick whatever the
    // rate does, and the phase changing is itself a change.
    if ((phase_ == EgPhase::Attack && att_ == 0) ||
        (phase_ == EgPhase::Decay && att_ >= sustain_att_))
      return to_tick;

    const int rate = rate_[static_cast<int>(phase_)];
    // Where the increment cannot land, no tick moves anything: the attack
    // update is guarded by `rate < 62`, the SSG-EG one by the fold level, and
    // the plain one saturates.
    const bool frozen =
        phase_ == EgPhase::Attack
            ? rate >= 62
            : (ssg_enable_ ? att_ >= kSsgFoldAttenuation
                           : att_ >= kMaxAttenuation);
    if (frozen)
      return detail::kUnboundedSkip;

    const int ticks = ticks_to_increment(rate);
    if (ticks == 0)
      return detail::kUnboundedSkip; // rate 0/1: the whole table row is zero
    return to_tick + static_cast<uint32_t>(ticks - 1) * kEgClockDivider;
  }

  // The output the current level carries at a given inversion.
  uint16_t output_with(bool inverted) const {
    int a = att_;
    if (inverted)
      a = (0x200 - a) & 0x3FF;
    const int o = a + (static_cast<int>(params_.tl & 0x7F) << 3);
    return static_cast<uint16_t>(o > kMaxAttenuation ? kMaxAttenuation : o);
  }

  // EG ticks from now until the first one whose table increment is non-zero;
  // 0 when the rate's row is all zero, so that no tick ever increments.
  int ticks_to_increment(int rate) const {
    const int shift = detail::rate_shift(rate);
    const int stride = 1 << shift;
    // The counter value the next tick will hold.
    const int start = (counter_ < 1 || counter_ >= 0x0FFF) ? 1 : counter_ + 1;
    // Only counters with the low `shift` bits clear reach the table at all.
    int c = (start + stride - 1) & ~(stride - 1);
    if (c > 0x0FFF)
      c = stride;
    for (int n = 0x0FFF / stride; n > 0; --n) {
      if (detail::kIncTable[rate][(c >> shift) & 7] != 0)
        return c >= start ? c - start + 1 : 0x1000 - start + c;
      c += stride;
      if (c > 0x0FFF)
        c = stride;
    }
    return 0;
  }

  // Runs once per output sample, before the envelope update.  The latches it
  // raises are consumed by the same sample; the key state and the phase it
  // reads are the ones this sample started with, so a key write that has not
  // been through a step() yet does not reach any of them.
  void ssg_step() {
    const bool in_fold = att_ >= kSsgFoldAttenuation;
    // Below the fold with the key state settled and the direction flag already
    // masked, every branch below is a no-op.
    if (!in_fold && keyed_on_ == keyed_on_at_start_ && (keyed_on_at_start_ || !ssg_invert_)) {
      ssg_in_fold_ = false;
      if (!keyed_on_)
        phase_ = EgPhase::Release;
      return;
    }
    // The fold region is every sample the envelope spends at or above 0x200,
    // which with AR < 31 is every sample of a whole attack.  The events report
    // the moment it arrives, not each sample it stays.
    const bool entering = in_fold && !ssg_in_fold_;
    ssg_in_fold_ = in_fold;

    bool repeat = false;
    bool direction = ssg_invert_;
    if (in_fold) {
      // Hold clear -> the ramp repeats, i.e. a key-on is re-asserted here.
      repeat = !ssg_hold_;
      // Alternate -> toggle inversion; alternate+hold -> force it set.
      if (ssg_alternate_)
        direction = ssg_hold_ ? true : !direction;
      // Neither alternate nor hold -> the phase generator is forced to 0.  We
      // do not model the PG; the moment is reported so the UI can mark it.
      if (!ssg_alternate_ && !ssg_hold_ && entering)
        events_ |= detail::kEvSsgPhaseReset;
    }
    // Modes 3 and 5 (hold set, attack and alternate differing) freeze at 0x200,
    // i.e. at full output volume, instead of cutting to silence below.
    const bool hold_up =
        keyed_on_ && ssg_hold_ && (ssg_attack_ != ssg_alternate_);
    direction = direction && keyed_on_at_start_;
    if (direction != ssg_invert_)
      events_ |= detail::kEvSsgInvert;
    ssg_invert_ = direction;

    const bool kon_event = (keyed_on_ && !keyed_on_at_start_) || (keyed_on_at_start_ && repeat);
    const bool koff_event = keyed_on_at_start_ && !keyed_on_;

    // The audible (inverted) level is latched in place, so release continues
    // from what was heard, not from the internal level.
    if (koff_event && (ssg_attack_ != direction))
      att_ = (kSsgFoldAttenuation - att_) & 0x3FF;
    const bool eg_off = att_ >= kSsgFoldAttenuation;

    if (kon_event) {
      // The virtual key-on: this is the 0x200 -> 0 snap.
      phase_ = EgPhase::Attack;
      if (rate_[0] >= 62)
        att_ = 0;
      if (entering && keyed_on_ && !ssg_hold_)
        events_ |= detail::kEvSsgFold;
    } else if (!keyed_on_) {
      phase_ = EgPhase::Release;
    }

    // Hold set -> the mode latches here, once.
    if (in_fold && ssg_hold_ && phase_at_start_ != EgPhase::Attack &&
        !ssg_held_) {
      ssg_held_ = true;
      events_ |= detail::kEvSsgHold;
    }

    envelope_off(kon_event, hold_up, eg_off);
  }

  // "Envelope off": with the slot out of Attack and no key-on re-asserted, a
  // level at the threshold forces att = 0x3FF and Release.  Evaluated per
  // output sample, so it lands on the sample after the tick that pushed att
  // there -- and the phase it tests is the one the sample started with, so a
  // slot that has yet to leave Attack is held for a sample first.
  void envelope_off(bool kon_event, bool hold_up, bool eg_off) {
    if (kon_event || hold_up || phase_at_start_ == EgPhase::Attack || !eg_off)
      return;
    att_ = kMaxAttenuation;
    phase_ = EgPhase::Release;
  }

  // Without SSG-EG the threshold is the top row of the scale instead of 0x200.
  void envelope_off_step() {
    envelope_off(keyed_on_ && !keyed_on_at_start_, false,
                 (att_ & 0x3F0) == 0x3F0 && att_ != kMaxAttenuation);
  }

  // One EG tick, clock / 432.
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
      if (rate < 62 && inc != 0 && keyed_on_)
        att_ += (~att_ * inc) >> 4;
      return;
    }

    if (ssg_enable_) {
      // 4x increment, frozen at 0x200.  Applies to DR, SR and RR alike.
      if (att_ < kSsgFoldAttenuation)
        att_ += 4 * inc;
    } else {
      att_ += inc;
    }
    if (att_ > kMaxAttenuation)
      att_ = kMaxAttenuation;
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
  EgPhase phase_at_start_ = EgPhase::Release;
  bool keyed_on_ = false;
  bool keyed_on_at_start_ = false;
  bool ssg_invert_ = false;
  bool ssg_held_ = false;
  bool ssg_in_fold_ = false;
  uint64_t samples_ = 0;
  uint32_t events_ = 0;
};

} // namespace ym2612_eg
