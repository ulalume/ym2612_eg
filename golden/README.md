# Golden vectors

114 cases recorded from Nuked-OPN2 (`ym3438.c` @ `335747d78cb0abbc3b55b004e62dad9763140115`)
at the register level: `eg_level`, `eg_state` and `eg_out` as change lists, plus the
EG-counter phase. Each file's `format` field describes its layout.
`test/golden_test.cpp` replays every case on `EgSimulator` with no tolerance; the
known model differences both sides apply are in `test/golden_common.hpp`.

## Regenerating

```sh
cmake -B build-gen -DCMAKE_BUILD_TYPE=Release -DYM2612_EG_BUILD_GOLDEN_GEN=ON
cmake --build build-gen --target golden      # rewrites golden/*.json in place
```

Nuked-OPN2 is LGPL 2.1; only `tools/golden_gen` links it.
