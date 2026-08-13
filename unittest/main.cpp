// main.cpp — AYVideo test runner entry point.
//
// Mirrors AYVoxel/unittest/main.cpp: defers to AYTest's runAllTests
// runner. argc/argv accepted-but-ignored (AYTest's runAllTests takes
// them by const-ref but does not parse them in V0.5).

#include "AYTest.h"

int main(int /*argc*/, char** /*argv*/) {
    return ayt::test::runAllTests("AYVideo");
}
