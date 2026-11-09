// Test-only controlled-fault resolver helper. It receives exactly the frozen
// hostname/service argv of the real wls861-resolve-ipv4 helper and speaks the
// same fixed WLS-RESOLVE-1 protocol, but selects its behaviour from the
// hostname: plain "localhost" answers, a "wls-count-N" hostname answers with
// exactly N distinct addresses, a "wls-fault-*" hostname plays one explicitly
// labelled protocol/lifecycle fault, and a "wls-probe-*" hostname reports one
// inherited-child hygiene fact through the same protocol. It performs no real
// name resolution, is staged into private 0700 fixture directories under the
// pinned helper name next to a staged copy of the test driver, and is never
// installed or a production path.
#define _GNU_SOURCE

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
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

std::string ok_addresses(int count) {
    std::string body = "WLS-RESOLVE-1\nok\n" + std::to_string(count) + "\n";
    for (int index = 0; index < count; ++index)
        body += "127.0.0." + std::to_string(index + 1) + "\n";
    return body;
}

// A clean child answers with one normal ok frame; any observed leak answers
// with a non-protocol marker so the parent sees ok=false for exactly the
// labelled probe and no other reason.
void probe(bool clean, const char* label) {
    if (clean) {
        emit(ok_addresses(1));
        return;
    }
    emit(std::string("PROBE-LEAK-") + label + "\n");
}

bool digit(const std::string& text, int& value) {
    if (text.empty() || text.size() > 2) return false;
    for (const char c : text)
        if (c < '0' || c > '9') return false;
    value = 0;
    for (const char c : text) value = value * 10 + (c - '0');
    return true;
}

} // namespace

extern char** environ;

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string hostname = argv[1];
    if (hostname == "localhost") {
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1\n");
        return 0;
    }
    if (hostname.rfind("wls-count-", 0) == 0) {
        int count = -1;
        if (digit(hostname.substr(10), count) && count >= 1 && count <= 16) {
            emit(ok_addresses(count));
            return 0;
        }
        return 1;
    }
    if (hostname == "wls-fault-fail") {
        emit("WLS-RESOLVE-1\nfail\n");
        return 0;
    }
    if (hostname == "wls-fault-fail-extra") {
        emit("WLS-RESOLVE-1\nfail\nEXTRA-BYTES\n");
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
    if (hostname == "wls-fault-noncanonical") {
        emit("WLS-RESOLVE-1\nok\n1\n127.1\n");
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
    if (hostname == "wls-fault-count-zero") {
        emit("WLS-RESOLVE-1\nok\n0\n");
        return 0;
    }
    if (hostname == "wls-fault-count-empty") {
        emit("WLS-RESOLVE-1\nok\n\n127.0.0.1\n");
        return 0;
    }
    if (hostname == "wls-fault-count-nonnum") {
        emit("WLS-RESOLVE-1\nok\n7x\n127.0.0.1\n");
        return 0;
    }
    // The count line says ten while the body carries one address.
    if (hostname == "wls-fault-count-transpose") {
        emit("WLS-RESOLVE-1\nok\n10\n127.0.0.1\n");
        return 0;
    }
    // The count line says sixty-one while the body carries sixteen addresses.
    if (hostname == "wls-fault-count-sixtyone") {
        std::string body = "WLS-RESOLVE-1\nok\n61\n";
        for (int index = 0; index < 16; ++index)
            body += "127.0.0." + std::to_string(index + 1) + "\n";
        emit(body);
        return 0;
    }
    // A leading zero is not a canonical strict decimal count.
    if (hostname == "wls-fault-count-leading-zero") {
        std::string body = "WLS-RESOLVE-1\nok\n01\n";
        for (int index = 0; index < 10; ++index)
            body += "127.0.0." + std::to_string(index + 1) + "\n";
        emit(body);
        return 0;
    }
    if (hostname == "wls-fault-extra") {
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1\nTRAILING\n");
        return 0;
    }
    if (hostname == "wls-fault-truncated") {
        emit("WLS-RESOLVE-1\nok\n1\n127.0.0.1"); // No final newline before EOF.
        return 0;
    }
    if (hostname == "wls-fault-oversize") {
        std::string body = "WLS-RESOLVE-1\nok\n16\n";
        for (int index = 0; index < 16; ++index) body += "123.123.123.123\n";
        body.append(300, 'x'); // Far beyond the 512-byte protocol bound.
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
    if (hostname == "wls-probe-argv") {
        probe(argc == 3 && std::string(argv[0]) == "wls861-resolve-ipv4", "argv");
        return 0;
    }
    if (hostname == "wls-probe-env") {
        probe(environ != nullptr && environ[0] == nullptr, "env");
        return 0;
    }
    if (hostname == "wls-probe-stdio") {
        struct stat stdin_stat {}, stdout_stat {}, stderr_stat {};
        const bool redirected = ::fstat(STDIN_FILENO, &stdin_stat) == 0 &&
            ::fstat(STDOUT_FILENO, &stdout_stat) == 0 &&
            ::fstat(STDERR_FILENO, &stderr_stat) == 0 &&
            S_ISCHR(stdin_stat.st_mode) && S_ISCHR(stderr_stat.st_mode) &&
            S_ISFIFO(stdout_stat.st_mode);
        probe(redirected, "stdio");
        return 0;
    }
    if (hostname == "wls-probe-fds") {
        bool closed = true;
        for (int fd = 3; fd < 64; ++fd)
            if (::fcntl(fd, F_GETFD) != -1) closed = false;
        probe(closed, "fds");
        return 0;
    }
    // The caller blocks SIGUSR2 before resolving; the spawned child must
    // arrive with an empty signal mask, not the parent's blocked set.
    if (hostname == "wls-probe-mask") {
        sigset_t current;
        ::sigemptyset(&current);
        const bool observed = ::sigprocmask(SIG_BLOCK, nullptr, &current) == 0;
        probe(observed && ::sigismember(&current, SIGUSR2) != 1, "mask");
        return 0;
    }
    if (hostname == "wls-probe-pdeathsig") {
        int signal_number = 0;
        probe(::prctl(PR_GET_PDEATHSIG, &signal_number, 0, 0, 0) == 0 &&
              signal_number == SIGKILL, "pdeathsig");
        return 0;
    }
    if (hostname == "wls-probe-nnp") {
        int active = 0;
        probe(::prctl(PR_GET_NO_NEW_PRIVS, &active, 0, 0, 0) == 0 && active == 1, "nnp");
        return 0;
    }
    if (hostname == "wls-probe-core") {
        struct rlimit core {};
        probe(::getrlimit(RLIMIT_CORE, &core) == 0 && core.rlim_cur == 0, "core");
        return 0;
    }
    return 1;
}
