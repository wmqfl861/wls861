// wls861-resolve-ipv4: pinned host resolver helper. Deployed next to the wls
// executable and invoked only through a pinned descriptor by the transport.
// It reads exactly one frozen hostname and one numeric service from argv,
// performs getaddrinfo(AF_INET, SOCK_STREAM), and writes the fixed protocol:
//   WLS-RESOLVE-1\nok\n<count>\n<strict IPv4>\n...  or  WLS-RESOLVE-1\nfail\n
// More than 16 distinct addresses, partial output, malformed results or an
// empty result are failures; output is never truncated to look complete.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

bool strict_address(const char* text, char (&canonical)[INET_ADDRSTRLEN]) {
    in_addr parsed{};
    return ::inet_pton(AF_INET, text, &parsed) == 1 &&
        ::inet_ntop(AF_INET, &parsed, canonical, sizeof(canonical)) != nullptr &&
        ::strcmp(canonical, text) == 0;
}

int write_all(const char* text, std::size_t size) {
    while (size != 0) {
        const auto written = ::write(STDOUT_FILENO, text, size);
        if (written <= 0) return 0;
        text += written;
        size -= static_cast<std::size_t>(written);
    }
    return 1;
}

int fail() {
    static constexpr char message[] = "WLS-RESOLVE-1\nfail\n";
    return write_all(message, sizeof(message) - 1) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const addrinfo hints{0, AF_INET, SOCK_STREAM, 0, 0, nullptr, nullptr, nullptr};
    addrinfo* result = nullptr;
    if (::getaddrinfo(argv[1], argv[2], &hints, &result) != 0) return fail();
    char addresses[16][INET_ADDRSTRLEN];
    std::size_t count = 0;
    for (auto* entry = result; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addrlen < sizeof(sockaddr_in))
            continue; // The AF_INET hint makes foreign families a protocol failure.
        char text[INET_ADDRSTRLEN];
        const auto* raw = ::inet_ntop(AF_INET,
            &reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr,
            text, sizeof(text));
        if (raw == nullptr) { ::freeaddrinfo(result); return fail(); }
        char canonical[INET_ADDRSTRLEN];
        if (!strict_address(raw, canonical)) { ::freeaddrinfo(result); return fail(); }
        bool duplicate = false;
        for (std::size_t index = 0; index < count; ++index)
            if (::strcmp(addresses[index], canonical) == 0) duplicate = true;
        if (duplicate) continue;
        if (count == 16) { // Over-limit never truncates to complete.
            ::freeaddrinfo(result);
            return fail();
        }
        ::memcpy(addresses[count++], canonical, sizeof(canonical));
    }
    ::freeaddrinfo(result);
    result = nullptr;
    if (count == 0) return fail();
    char output[512];
    int used = ::snprintf(output, sizeof(output), "WLS-RESOLVE-1\nok\n%zu\n", count);
    for (std::size_t index = 0; index < count && used > 0; ++index)
        used += ::snprintf(output + used, sizeof(output) - static_cast<std::size_t>(used),
                           "%s\n", addresses[index]);
    if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(output)) return 1;
    if (!write_all(output, static_cast<std::size_t>(used))) return 1;
    return 0;
}
