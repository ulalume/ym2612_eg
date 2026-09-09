cmake_minimum_required(VERSION 3.20)
project(ym2612_eg VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(YM2612_EG_BUILD_TESTS "Build tests" OFF)
option(YM2612_EG_BUILD_BENCH "Build the sample_curve benchmark" OFF)
# The generator links Nuked-OPN2 (LGPL 2.1); the library and its tests never do.
option(YM2612_EG_BUILD_GOLDEN_GEN "Build the Nuked-OPN2 golden-vector generator" OFF)

# --- Library (header-only) ---
add_library(ym2612_eg INTERFACE)
add_library(ym2612_eg::ym2612_eg ALIAS ym2612_eg)

target_include_directories(ym2612_eg INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(ym2612_eg INTERFACE cxx_std_17)

# --- Tests ---
if(YM2612_EG_BUILD_TESTS)
  enable_testing()

  foreach(_t eg_test ssg_test curve_test timing_test graph_test handles_test)
    add_executable(ym2612_eg_${_t} test/${_t}.cpp)
    target_link_libraries(ym2612_eg_${_t} PRIVATE ym2612_eg)
    target_include_directories(ym2612_eg_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    add_test(NAME ${_t} COMMAND ym2612_eg_${_t})
  endforeach()

  # Golden vectors: committed JSON recorded from Nuked-OPN2 by
  # tools/golden_gen.  This target only reads that data -- no Nuked here.
  add_executable(ym2612_eg_golden_test test/golden_test.cpp)
  target_link_libraries(ym2612_eg_golden_test PRIVATE ym2612_eg)
  target_include_directories(ym2612_eg_golden_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)

  file(GLOB _golden_vectors RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/golden
       ${CMAKE_CURRENT_SOURCE_DIR}/golden/*.json)
  if(NOT _golden_vectors)
    message(FATAL_ERROR "golden/*.json is empty; regenerate with -DYM2612_EG_BUILD_GOLDEN_GEN=ON")
  endif()
  foreach(_v ${_golden_vectors})
    get_filename_component(_name ${_v} NAME_WE)
    add_test(NAME golden_${_name}
             COMMAND ym2612_eg_golden_test ${CMAKE_CURRENT_SOURCE_DIR}/golden/${_v})
  endforeach()
endif()

# --- Golden-vector generator (links Nuked-OPN2, LGPL 2.1) ---
if(YM2612_EG_BUILD_GOLDEN_GEN)
  add_subdirectory(tools/golden_gen)
endif()

# --- Benchmark ---
if(YM2612_EG_BUILD_BENCH)
  add_executable(ym2612_eg_bench bench/curve_bench.cpp)
  target_link_libraries(ym2612_eg_bench PRIVATE ym2612_eg)
endif()
