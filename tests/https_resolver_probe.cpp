// No-seam production resolver probe: links only the production wls_s1 object
// (never the test-seam compilation unit), so install and relocation
// regressions exercise the shipped parent-side lifecycle against the real
// installed helper bytes. Not installed, not a CTest entry of its own; the
// runtime-resources gate stages a private copy next to the helper under test.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../src/https_resolver_internal.hpp"

#include <chrono>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "--resolve-once") return 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool threw = false;
    wls::s1::resolver::Observation observation;
    try {
        observation = wls::s1::resolver::resolve_ipv4(argv[2], argv[3], deadline, {});
    } catch (const wls::s1::Error&) {
        threw = true;
    }
    const std::string first = observation.addresses.empty() ? std::string("-") :
        observation.addresses.front();
    std::printf("RESULT threw=%d started=%d ok=%d pending=%d addr=%zu first=%s identity=%d\n",
                threw ? 1 : 0, observation.started ? 1 : 0, observation.ok ? 1 : 0,
                observation.cleanup_pending ? 1 : 0, observation.addresses.size(),
                first.c_str(), observation.helper_identity.empty() ? 0 : 1);
    return (!threw && observation.ok && !observation.cleanup_pending) ? 0 : 1;
}
