// Test-special compilation unit for the resolver lifecycle: builds the real
// production source with WLS_RESOLVER_TEST_SEAMS so the controlled fault and
// pending-seam hooks of tests/https_resolver_seams.hpp are active. This unit
// is only ever linked into the dedicated https_resolver_tests driver; the
// wls_s1 library object, wls/wls861 and wls861-resolve-ipv4 compile the same
// source without any test switch.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "https_resolver_seams.hpp"

// Matches the Linux libc C declaration and exception specification. This
// strong symbol belongs only to the dedicated test executable. It delegates
// directly to SYS_clock_gettime except for the scoped subms thread/PID;
// libraries in that armed thread see the same explicit controlled test time.
extern "C" int clock_gettime(clockid_t clock, timespec* value) noexcept {
    return wls::s1::resolver::seams::clock_gettime_observed(clock, value);
}

#define WLS_RESOLVER_TEST_SEAMS
#include "../src/https_resolver_posix.cpp"
