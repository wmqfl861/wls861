// Test-special compilation unit for the resolver lifecycle: builds the real
// production source with WLS_RESOLVER_TEST_SEAMS so the controlled fault and
// pending-seam hooks of tests/https_resolver_seams.hpp are active. This unit
// is only ever linked into the dedicated https_resolver_tests driver; the
// wls_s1 library object, wls/wls861 and wls861-resolve-ipv4 compile the same
// source without any test switch.
#define WLS_RESOLVER_TEST_SEAMS
#include "../src/https_resolver_posix.cpp"
