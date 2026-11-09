// Test-only controlled-fault resolver helper. It receives exactly the frozen
// hostname/service argv of the real wls861-resolve-ipv4 helper and speaks the
// same fixed WLS-RESOLVE-1 protocol, but selects its behaviour from the
// hostname: plain "localhost" answers, while a "wls-fault-*" hostname plays
// one explicitly labelled protocol/lifecycle fault. It performs no real name
// resolution, is copied into private 0700 fixture directories under the
// pinned helper name, and is never installed or a production path.
#define _GNU_SOURCE

#include <unistd.h>

#include <string>

namespace {

void emit(const std::string& text) {
    (void)::write(STDOUT_FILENO, text.data(), text.size());
}

void sleep_ms(int milliseconds) {
    const timespec gap{milliseconds / 1000, (milliseconds % 1000) * 1000000};
    ::nanosleep(&gap, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string hostname = argv[1];
    if (hostname == "localhost") {
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1\n");
        return 0;
    }
    if (hostname == "wls-fault-fail") {
        emit("WLS-RESOLVE-1\nfail\n");
        return 0;
    }
    if (hostname == "wls-fault-malformed") {
        emit("THIS-IS-NOT-THE-PROTOCOL\r\nrandom bytes\x01\x02\n");
        return 0;
    }
    if (hostname == "wls-fault-partial") {
        emit("WLS-RESOLVE-1\nok\n3\n127.0.0.1\n"); // Declares 3, delivers 1, then EOF.
        return 0;
    }
    if (hostname == "wls-fault-invalid") {
        emit("WLS-RESOLVE-1\nok\n1\nnot-an-address\n");
        return 0;
    }
    if (hostname == "wls-fault-duplicate") {
        emit("WLS-RESOLVE-1\nok\n2\n127.0.0.1\n127.0.0.1\n");
        return 0;
    }
    if (hostname == "wls-fault-overlimit") {
        std::string body = "WLS-RESOLVE-1\nok\n17\n";
        for (int index = 0; index < 17; ++index)
            body += "10.0.0." + std::to_string(index + 1) + "\n";
        emit(body);
        return 0;
    }
    if (hostname == "wls-fault-nonzero") {
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1\n");
        return 3;
    }
    if (hostname == "wls-fault-close") return 0;
    if (hostname == "wls-fault-delay") {
        sleep_ms(1500);
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1\n");
        return 0;
    }
    if (hostname == "wls-fault-block") {
        for (;;) pause();
    }
    return 1;
}
