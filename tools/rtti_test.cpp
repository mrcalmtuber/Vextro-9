/*
 * tools/rtti_test.cpp — the same dynamic_cast questions, asked of the
 * host's C++ runtime.
 *
 * This is one half of a differential test and it is the half that
 * carries no Vextro code at all: it is compiled against the host's own
 * headers and links the host's own libc++ or libstdc++, so the answers
 * it produces are a *reference* implementation's answers.
 *
 * The other half is apps/cxxtest.cpp, which compiles the identical
 * apps/rtti_cases.h in ring 3 over libcxx/src/typeinfo.cpp. The two
 * must agree on all 43, and every expectation in that header is an
 * address the compiler works out statically -- so neither run is
 * checking an implementation against itself.
 *
 * It runs on every `make test`, which is where it earns its place: a
 * dynamic_cast bug is silent, and finding it here takes a second where
 * finding it on the machine takes a boot.
 */

#include <cstdio>
#include "rtti_cases.h"

static int checks = 0, failures = 0;

static void check(const char *what, bool good) {
    checks++;
    if (!good) {
        failures++;
        std::printf("  FAIL  %s\n", what);
    }
}

int main() {
    rtti_cases::run(check);
    std::printf("  ok   rtti: %d dynamic_cast and typeid cases, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
