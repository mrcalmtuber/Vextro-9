/*
 * apps/rtti_probe.cpp — the one translation unit in this repository
 * compiled with RTTI on.
 *
 * Everything else here is built -fno-rtti, and apps/cxxtest.cpp with it,
 * so the dynamic_cast and typeid cases cannot live in that file: the
 * flag is per translation unit and a `typeid` under -fno-rtti is a
 * compile error rather than a link one.
 *
 * So the cases are in apps/rtti_cases.h, this file compiles them with
 * -frtti, and cxxtest calls the single function below. That is also the
 * arrangement ICU is built with -- one library compiled -frtti, linked
 * into programs that are not -- so this file checks the mixture as well
 * as the casts.
 */

#include "rtti_cases.h"

extern "C" void vx_rtti_run(void (*check)(const char *what, bool good)) {
    rtti_cases::run(check);
}
