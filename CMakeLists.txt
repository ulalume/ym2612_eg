cmake_minimum_required(VERSION 3.20)
project(ym2612_eg VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(YM2612_EG_BUILD_TESTS "Build tests" OFF)
option(YM2612_EG_BUILD_BENCH "Build the sample_curve benchmark" OFF)

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

  foreach(_t eg_test ssg_test curve_test)
    add_executable(ym2612_eg_${_t} test/${_t}.cpp)
    target_link_libraries(ym2612_eg_${_t} PRIVATE ym2612_eg)
    target_include_directories(ym2612_eg_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    add_test(NAME ${_t} COMMAND ym2612_eg_${_t})
  endforeach()
endif()

# --- Benchmark ---
if(YM2612_EG_BUILD_BENCH)
  add_executable(ym2612_eg_bench bench/curve_bench.cpp)
  target_link_libraries(ym2612_eg_bench PRIVATE ym2612_eg)
endif()
