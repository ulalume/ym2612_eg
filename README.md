# ym2612_eg

Sample-accurate YM2612 (OPN2) envelope generator for drawing envelopes.
Header-only C++17, no dependencies. Not an audio emulator.

## Usage

```cpp
#include <ym2612_eg/ym2612_eg.hpp>
using namespace ym2612_eg;

CurveRequest req;
req.op = OperatorParams{31, 10, 5, 7, 2, 0, 0, 0}; // AR DR SR RR SL TL KS SSG
req.pitch = NotePitch{644, 5};                     // F-num, block
req.gate_ms = 500.0;                               // < 0: never released
req.max_ms = 2000.0;
const CurveResult curve = sample_curve(req);       // points, markers, warnings

const PhaseDurations d = phase_durations(req.op, req.pitch);
const double loop_ms = ssg_loop_period_ms(req.op, req.pitch);

using namespace ym2612_eg::graph;
const EnvelopeCurve c = build_envelope_curve(req.op, req.pitch);
const VoiceCursor v = cursor_for_voice(c, since_key_on_ms, since_key_off_ms, c.span_ms);
```

## CMake

```cmake
include(FetchContent)
FetchContent_Declare(ym2612_eg
  GIT_REPOSITORY https://github.com/ulalume/ym2612_eg.git
  GIT_TAG v0.3.0)
FetchContent_MakeAvailable(ym2612_eg)
target_link_libraries(your_target PRIVATE ym2612_eg)
```

Tests: `cmake -B build -DYM2612_EG_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build`

## License

MIT. `tools/golden_gen` links Nuked-OPN2 (LGPL 2.1); it is a development tool
built only with `YM2612_EG_BUILD_GOLDEN_GEN=ON`, and no part of it is in the library.
