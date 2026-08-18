#include "TestHarness.h"

// Single entry point for every unit-test translation unit. Tests register
// themselves at static-initialization time, so adding a file to the target is
// all that is needed to run its tests.
int main() { return optiforge::test::runAll(); }
