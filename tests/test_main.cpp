#include <cstdio>

#include "test_support.h"

void runGlFilterTests();
void runGovernorTests();
void runTelemetryTests();
void runPolicyTests();

int main() {
  runGlFilterTests();
  runGovernorTests();
  runTelemetryTests();
  runPolicyTests();

  if (test::failures() == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d test(s) failed\n", test::failures());
  return 1;
}