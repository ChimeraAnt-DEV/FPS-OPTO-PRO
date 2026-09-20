#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Minimal assertion helpers so the tests depend on nothing but the standard
// library. Every failure is reported and the process exits non-zero at the end,
// so one run shows all the broken expectations rather than only the first.
namespace test {

inline int &failures() {
  static int count = 0;
  return count;
}

inline void reportFailure(const char *file, int line, const std::string &what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
  ++failures();
}

template <typename A, typename B>
void expectEqual(const char *file, int line, const char *expr, const A &actual,
                 const B &expected) {
  if (!(actual == expected)) {
    reportFailure(file, line,
                  std::string(expr) + ": expected " +
                      std::to_string(static_cast<long long>(expected)) +
                      ", got " +
                      std::to_string(static_cast<long long>(actual)));
  }
}

inline void expectTrue(const char *file, int line, const char *expr, bool value) {
  if (!value) {
    reportFailure(file, line, std::string(expr) + " should be true");
  }
}

inline void expectFalse(const char *file, int line, const char *expr, bool value) {
  if (value) {
    reportFailure(file, line, std::string(expr) + " should be false");
  }
}

inline void expectNear(const char *file, int line, const char *expr, double actual,
                       double expected, double tolerance) {
  if (!(std::fabs(actual - expected) <= tolerance)) {
    reportFailure(file, line,
                  std::string(expr) + ": expected ~" + std::to_string(expected) +
                      ", got " + std::to_string(actual));
  }
}

} // namespace test

// The macros below read a little unusually on purpose: they are variadic-free
// by design so that an argument containing a comma cannot silently change the
// number of parameters.
#define EXPECT_EQ(actual, expected) \
  test::expectEqual(__FILE__, __LINE__, #actual, (actual), (expected))
#define EXPECT_TRUE(value) test::expectTrue(__FILE__, __LINE__, #value, (value))
#define EXPECT_FALSE(value) test::expectFalse(__FILE__, __LINE__, #value, (value))
#define EXPECT_NEAR(actual, expected, tolerance) \
  test::expectNear(__FILE__, __LINE__, #actual, (actual), (expected), (tolerance))
