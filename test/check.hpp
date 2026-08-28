#pragma once

// Minimal assert-style harness (same spirit as ym2612_format's roundtrip_test).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace testing {

inline int g_tests = 0;
inline int g_failed_tests = 0;
inline int g_checks = 0;
inline int g_failed_checks = 0;
inline bool g_current_ok = true;

inline void fail(const char *file, int line, const std::string &what) {
  ++g_failed_checks;
  g_current_ok = false;
  std::cerr << "\n    FAIL " << file << ":" << line << "  " << what;
}

inline int summary() {
  std::cout << "\n" << g_tests << " tests, " << g_checks << " checks, "
            << g_failed_tests << " failed tests, " << g_failed_checks
            << " failed checks\n";
  return g_failed_tests == 0 ? 0 : 1;
}

} // namespace testing

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++testing::g_checks;                                                       \
    if (!(cond))                                                               \
      testing::fail(__FILE__, __LINE__, #cond);                                \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    ++testing::g_checks;                                                       \
    const auto _a = (a);                                                       \
    const auto _b = (b);                                                       \
    if (!(_a == _b))                                                           \
      testing::fail(__FILE__, __LINE__,                                        \
                    std::string(#a) + " == " + #b + "  (got " +                \
                        std::to_string(static_cast<long long>(_a)) + " vs " +  \
                        std::to_string(static_cast<long long>(_b)) + ")");     \
  } while (0)

// Relative tolerance, expressed as a fraction (0.01 == +/-1%).
#define CHECK_REL(actual, expected, frac)                                      \
  do {                                                                         \
    ++testing::g_checks;                                                       \
    const double _x = static_cast<double>(actual);                             \
    const double _e = static_cast<double>(expected);                           \
    if (!(std::fabs(_x - _e) <= std::fabs(_e) * (frac)))                       \
      testing::fail(__FILE__, __LINE__,                                        \
                    std::string(#actual) + " ~= " + #expected + "  (got " +    \
                        std::to_string(_x) + ", want " + std::to_string(_e) +  \
                        " +/- " + std::to_string((frac) * 100.0) + "%)");      \
  } while (0)

#define CHECK_ABS(actual, expected, tol)                                       \
  do {                                                                         \
    ++testing::g_checks;                                                       \
    const double _x = static_cast<double>(actual);                             \
    const double _e = static_cast<double>(expected);                           \
    if (!(std::fabs(_x - _e) <= (tol)))                                        \
      testing::fail(__FILE__, __LINE__,                                        \
                    std::string(#actual) + " ~= " + #expected + "  (got " +    \
                        std::to_string(_x) + ", want " + std::to_string(_e) +  \
                        " +/- " + std::to_string(static_cast<double>(tol)) +   \
                        ")");                                                  \
  } while (0)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    ++testing::g_tests;                                                        \
    testing::g_current_ok = true;                                              \
    std::cout << "  " << #fn << " ... " << std::flush;                         \
    fn();                                                                      \
    if (testing::g_current_ok) {                                               \
      std::cout << "ok\n";                                                     \
    } else {                                                                   \
      ++testing::g_failed_tests;                                               \
      std::cout << "\n  " << #fn << " FAILED\n";                               \
    }                                                                          \
  } while (0)
