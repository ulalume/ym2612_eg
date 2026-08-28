# Golden-vector generator.
#
# LICENSING: Nuked-OPN2 is LGPL 2.1 while ym2612_eg is MIT.  It is fetched and
# compiled here and nowhere else -- this directory is only added when
# YM2612_EG_BUILD_GOLDEN_GEN is ON, so neither the library nor the CTest suite
# ever links it.  What ships is golden/*.json, which is data.

# ym3438.c is C; the library itself is CXX-only, so C is enabled just here.
enable_language(C)

include(FetchContent)

FetchContent_Declare(nuked_opn2
  GIT_REPOSITORY https://github.com/nukeykt/Nuked-OPN2.git
  # Keep in sync with kNukedCommit in test/golden_common.hpp.
  GIT_TAG 335747d78cb0abbc3b55b004e62dad9763140115
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(nuked_opn2)

add_library(nuked_opn2 STATIC ${nuked_opn2_SOURCE_DIR}/ym3438.c)
target_include_directories(nuked_opn2 PUBLIC ${nuked_opn2_SOURCE_DIR})


add_executable(ym2612_eg_golden_gen main.cpp)
target_link_libraries(ym2612_eg_golden_gen PRIVATE ym2612_eg nuked_opn2)
target_include_directories(ym2612_eg_golden_gen
  PRIVATE ${CMAKE_SOURCE_DIR}/test)

# `cmake --build build --target golden` regenerates golden/*.json in place.
add_custom_target(golden
  COMMAND ym2612_eg_golden_gen ${CMAKE_SOURCE_DIR}/golden
  DEPENDS ym2612_eg_golden_gen
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Regenerating golden/*.json from Nuked-OPN2")
