// Golden-vector test: replays a recorded Nuked-OPN2 scenario on EgSimulator and
// compares tick by tick.
//
// The vectors in golden/*.json were produced by tools/golden_gen, which links
// Nuked-OPN2 (LGPL 2.1).  Nothing here does -- this reads committed data.
//
//   ym2612_eg_golden_test <path-to-vector.json>
//
// Comparison policy (see golden/DISCREPANCIES.md for the evidence):
//   * eg_level vs attenuation()  -- exact, at every output sample.
//   * eg_out   vs output()       -- exact, at every sample, allowing for
//                                   Nuked's one-sample eg_out pipeline lag,
//                                   which the generator already removed.
//   * eg_state vs phase()        -- exact, but only on EG-tick samples.  Nuked
//                                   runs its state machine once per output
//                                   sample and moves one state per sample,
//                                   while this library evaluates the whole
//                                   transition chain at the top of an EG tick;
//                                   the two agree at every tick.
//   * counter_shift              -- recomputed here from the case's own rates
//                                   and required to match what the file says,
//                                   so a vector cannot ship a fitted alignment.

#include <ym2612_eg/ym2612_eg.hpp>

#include "check.hpp"
#include "golden_common.hpp"
#include "json_lite.hpp"

#include <string>
#include <vector>

using namespace ym2612_eg;
using namespace ym2612_eg::golden;

namespace {

// Expand a flat [sample, value, ...] change list into a per-sample array.
std::vector<int> expand(const std::vector<double> &changes, int samples) {
  std::vector<int> out(static_cast<size_t>(samples), 0);
  if (changes.empty())
    return out;
  CHECK_EQ(changes.size() % 2, 0u);
  size_t k = 0;
  int value = 0;
  for (int i = 0; i < samples; ++i) {
    while (k + 1 < changes.size() && static_cast<int>(changes[k]) == i) {
      value = static_cast<int>(changes[k + 1]);
      k += 2;
    }
    out[static_cast<size_t>(i)] = value;
  }
  CHECK_EQ(k, changes.size());
  return out;
}

std::string g_file;
int g_cases = 0;

void run_case(const jsonlite::Value &c) {
  const std::string name = c.str("name");
  OperatorParams op;
  op.ar = static_cast<uint8_t>(c.integer("ar"));
  op.dr = static_cast<uint8_t>(c.integer("dr"));
  op.sr = static_cast<uint8_t>(c.integer("sr"));
  op.rr = static_cast<uint8_t>(c.integer("rr"));
  op.sl = static_cast<uint8_t>(c.integer("sl"));
  op.tl = static_cast<uint8_t>(c.integer("tl"));
  op.ks = static_cast<uint8_t>(c.integer("ks"));
  op.ssg = static_cast<uint8_t>(c.integer("ssg"));
  const NotePitch pitch{static_cast<uint16_t>(c.integer("fnum")),
                        static_cast<uint8_t>(c.integer("block"))};

  const int samples = static_cast<int>(c.integer("samples"));
  const int counter_phase = static_cast<int>(c.integer("counter_phase"));
  const int stored_shift = static_cast<int>(c.integer("counter_shift"));

  EgSimulator eg(op, pitch);

  // The alignment is a rule, not a fitted constant: derive it here and refuse
  // the vector if it disagrees with what the generator recorded.
  bool mixed = false;
  int shift = counter_shift_for_case(eg, &mixed);
  if (shift == kNoConstraint)
    shift = 0;
  if (mixed || shift != stored_shift) {
    testing::fail(__FILE__, __LINE__,
                  g_file + " / " + name + ": counter_shift " +
                      std::to_string(stored_shift) + " is not the shift the " +
                      "documented eg_timer_low_lock rule derives (" +
                      (mixed ? std::string("mixed rates")
                             : std::to_string(shift)) +
                      ")");
    return;
  }
  ++testing::g_checks;

  // Combinations this library is known to disagree with must never reach a
  // vector; golden/DISCREPANCIES.md explains each one.
  const char *excluded = nullptr;
  if (has_sl0_instant_attack_divergence(eg))
    excluded = "SL=0 with instant attack";
  else if (has_ssg_sustain_window_divergence(op, eg))
    excluded = "SSG-EG decay step wide enough to jump Nuked's sustain window";
  else if (shift != 0 && counter_wraps(counter_phase, samples))
    excluded = "counter-shifted case that outlives one 12-bit counter sweep";
  if (excluded) {
    testing::fail(__FILE__, __LINE__,
                  g_file + " / " + name + ": " + excluded +
                      " is a documented divergence and must not appear in a "
                      "golden vector");
    return;
  }
  ++testing::g_checks;

  const std::vector<int> level = expand(c.nums("level"), samples);
  const std::vector<int> state = expand(c.nums("state"), samples);
  // out is recorded only when it differs from level; and it is only meaningful
  // at all when nothing can flip the SSG inversion mid-trace (see
  // golden_common.hpp).
  const bool check_out = output_comparable(op);
  const std::vector<int> out =
      c.find("out") ? expand(c.nums("out"), samples) : level;
  const std::vector<double> &gate = c.nums("gate");

  eg.reset(static_cast<uint16_t>(apply_counter_shift(counter_phase, shift)));

  int bad_level = 0, bad_state = 0, bad_out = 0;
  int first_bad = -1;
  size_t g = 0;
  for (int i = 0; i < samples; ++i) {
    while (g + 1 < gate.size() && static_cast<int>(gate[g]) == i) {
      if (gate[g + 1] != 0) {
        eg.key_on();
      } else {
        // Two divergences only a key-off can expose, both evaluated on the
        // envelope as it stands going into it.
        const char *bad = nullptr;
        if (has_ssg_alternate_keyon_parity_divergence(op, eg))
          bad = "SSG-EG alternate without hold under an attack below rate 62";
        else if (has_ssg_keyoff_cut_divergence(op, eg))
          bad = "SSG-EG key-off with the latched level at or above 0x200";
        if (bad) {
          testing::fail(__FILE__, __LINE__,
                        g_file + " / " + name + ": " + bad +
                            " is a documented divergence and must not appear "
                            "in a golden vector (key-off at sample " +
                            std::to_string(i) + ")");
          return;
        }
        eg.key_off();
      }
      g += 2;
    }
    eg.step();

    if (eg.attenuation() != level[static_cast<size_t>(i)]) {
      if (first_bad < 0)
        first_bad = i;
      ++bad_level;
    }
    // Nuked's eg_out for the last sample would need one more sample of trace,
    // so the generator stops the output list one sample short.
    if (check_out && i + kOutputLagSamples < samples &&
        eg.output() != out[static_cast<size_t>(i)]) {
      if (first_bad < 0)
        first_bad = i;
      ++bad_out;
    }
    if (i % 3 == 0 &&
        static_cast<int>(eg.phase()) != state[static_cast<size_t>(i)]) {
      if (first_bad < 0)
        first_bad = i;
      ++bad_state;
    }
  }
  CHECK_EQ(gate.size(), g);

  ++testing::g_checks;
  if (bad_level || bad_state || bad_out) {
    const size_t i = static_cast<size_t>(first_bad);
    EgSimulator probe(op, pitch);
    testing::fail(
        __FILE__, __LINE__,
        g_file + " / " + name + ": " + std::to_string(bad_level) +
            " level, " + std::to_string(bad_out) + " output and " +
            std::to_string(bad_state) + " state mismatches over " +
            std::to_string(samples) + " samples; first at sample " +
            std::to_string(first_bad) + " (Nuked level " +
            std::to_string(level[i]) + ", state " + std::to_string(state[i]) +
            "); rates " + std::to_string(probe.rate_of(EgPhase::Attack)) + "/" +
            std::to_string(probe.rate_of(EgPhase::Decay)) + "/" +
            std::to_string(probe.rate_of(EgPhase::Sustain)) + "/" +
            std::to_string(probe.rate_of(EgPhase::Release)));
  }
  ++g_cases;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <vector.json>\n";
    return 2;
  }
  g_file = argv[1];
  const size_t slash = g_file.find_last_of('/');
  const std::string base =
      slash == std::string::npos ? g_file : g_file.substr(slash + 1);

  const jsonlite::Value root = jsonlite::parse_file(g_file);
  const std::string expected_source =
      "Nuked-OPN2 ym3438.c @ " + std::string(kNukedCommit);
  if (root.str("source") != expected_source) {
    std::cerr << g_file << ": recorded from \"" << root.str("source")
              << "\" but golden_common.hpp pins \"" << expected_source
              << "\"\n";
    return 1;
  }

  const jsonlite::Value &cases = root.at("cases");
  if (cases.type != jsonlite::Value::Type::Array || cases.array.empty()) {
    std::cerr << g_file << ": no cases\n";
    return 1;
  }

  ++testing::g_tests;
  testing::g_current_ok = true;
  std::cout << "  " << base << " (" << cases.array.size() << " cases) ... "
            << std::flush;
  for (const jsonlite::Value &c : cases.array)
    run_case(c);
  if (testing::g_current_ok) {
    std::cout << "ok\n";
  } else {
    ++testing::g_failed_tests;
    std::cout << "\n  " << base << " FAILED\n";
  }

  return testing::summary();
}
