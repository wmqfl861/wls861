// Parent-side bounded lifecycle for the pinned wls861-resolve-ipv4 helper.
// Linux-only. Discovery is executable-relative only; each path component is
// pinned with O_PATH|O_NOFOLLOW and the helper must be a regular ELF owned by
// root or the effective user, without setuid/setgid or group/world write.
// The forked child performs only async-signal-safe calls before execveat on
// the pinned descriptor. The parent reads a bounded nonblocking pipe under
// the caller's single deadline, requires EOF plus a reaped exit 0, and on
// cancel/error kills via pidfd (or the owned pid) within a bounded cleanup
// window; an unreaped child stays pending and blocks further live resolves.
#define _GNU_SOURCE

#include "https_resolver_internal.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <mutex>
#include <thread>
#include <utility>

namespace wls::s1::resolver {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char helper_name[] = "wls861-resolve-ipv4";
constexpr int cleanup_window_ms = 25;
constexpr int poll_slice_ms = 25;

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}

struct PendingChild {
    pid_t pid{-1};
    int pidfd{-1};
};

// Process-local slot holding at most one unreaped child. A still-pending
// child blocks new live resolver use; its pid/pidfd are never dropped.
std::mutex slot_mutex;
PendingChild slot;

struct PinnedHelper {
    int exec_fd{-1};
    std::string path;
    std::string identity;
    ~PinnedHelper() { if (exec_fd >= 0) ::close(exec_fd); }
};

std::string self_executable() {
    std::array<char, 4096> buffer{};
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    require(size > 0, ErrorCode::Unsupported,
            "resolver helper discovery requires /proc/self/exe");
    return std::string(buffer.data(), static_cast<std::size_t>(size));
}

// Walks each component below / with O_PATH|O_NOFOLLOW, then pins the helper
// leaf. Symlinks, special files, wrong ownership or permissive modes are
// refused before any byte of the helper is read or executed.
PinnedHelper pin_helper() {
    const auto executable = self_executable();
    const auto slash = executable.find_last_of('/');
    require(slash != std::string::npos && slash != 0, ErrorCode::Unsupported,
            "resolver helper discovery found no executable directory");
    const auto directory = executable.substr(0, slash);
    int dirfd = ::open("/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(dirfd >= 0, ErrorCode::Unsupported, "resolver helper root could not be pinned");
    std::size_t start = 1;
    while (start < directory.size()) {
        const auto end = directory.find('/', start);
        const auto component = directory.substr(start, (end == std::string::npos ?
            directory.size() : end) - start);
        const auto next = ::openat(dirfd, component.c_str(),
            O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(dirfd);
        require(next >= 0, ErrorCode::Unsupported,
                "resolver helper directory could not be pinned without symlink descent");
        dirfd = next;
        start = end == std::string::npos ? directory.size() : end + 1;
    }
    const int path_fd = ::openat(dirfd, helper_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    ::close(dirfd);
    require(path_fd >= 0, ErrorCode::Unsupported,
            "the pinned resolver helper is missing next to the executable");
    struct stat pinned {};
    require(::fstat(path_fd, &pinned) == 0, ErrorCode::Unsupported,
            "the pinned resolver helper could not be observed");
    require(S_ISREG(pinned.st_mode) &&
            (pinned.st_uid == 0 || pinned.st_uid == ::geteuid()) &&
            (pinned.st_mode & (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) == 0,
            ErrorCode::Unsupported,
            "the resolver helper must be a regular non-setuid file without shared writes");
    const auto exec_fd = ::openat(dirfd, helper_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(exec_fd >= 0, ErrorCode::Unsupported,
            "the pinned resolver helper could not be opened for execution");
    struct stat opened {};
    require(::fstat(exec_fd, &opened) == 0 && opened.st_dev == pinned.st_dev &&
            opened.st_ino == pinned.st_ino,
            ErrorCode::Unsupported, "the resolver helper changed while being pinned");
    std::array<char, 4> elf{};
    require(::pread(exec_fd, elf.data(), elf.size(), 0) == 4 && elf[0] == '\x7f' &&
            elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F',
            ErrorCode::Unsupported, "the resolver helper is not an ELF executable");
    PinnedHelper helper;
    helper.exec_fd = exec_fd;
    helper.path = directory + "/" + helper_name;
    helper.identity = helper.path + "|bytes=" + std::to_string(pinned.st_size) +
        "|mode=0" + std::to_string(pinned.st_mode & 0777777);
    return helper;
}

// pidfd-first kill of this process's own child; falls back to the owned pid
// when the kernel or pidfd allocation refuses.
void kill_child(pid_t pid, int& pidfd) noexcept {
    if (pidfd < 0) {
#ifdef SYS_pidfd_open
        pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#endif
    }
    bool sent = false;
#ifdef SYS_pidfd_send_signal
    if (pidfd >= 0)
        sent = ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0) == 0;
#endif
    if (!sent) (void)::kill(pid, SIGKILL);
}

// Bounded non-blocking reap loop; waits for pidfd readiness in small slices
// and never blocks past `until`.
bool reap_bounded(pid_t pid, int pidfd, Clock::time_point until) noexcept {
    while (true) {
        const auto reaped = ::waitpid(pid, nullptr, WNOHANG);
        if (reaped == pid) return true;
        if (reaped != 0) return false;
        const auto now = Clock::now();
        if (now >= until) return false;
        pollfd descriptor{static_cast<short>(pidfd >= 0 ? pidfd : 0),
                          static_cast<short>(pidfd >= 0 ? POLLIN : 0), 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - now).count();
        (void)::poll(&descriptor, 1, static_cast<int>(
            std::max<std::int64_t>(1, std::min<std::int64_t>(5, remaining))));
    }
}

// Strict parser for the fixed helper protocol. Rejects partial lines, extra
// bytes, duplicate or non-canonical addresses and out-of-range counts.
bool parse_response(const std::string& bytes, std::vector<std::string>& addresses) {
    std::size_t start = 0;
    const auto line = [&]() -> std::string_view {
        const auto end = bytes.find('\n', start);
        if (end == std::string::npos) return {};
        const auto view = std::string_view(bytes).substr(start, end - start);
        start = end + 1;
        return view;
    };
    if (line() != std::string_view(magic)) return false;
    const auto status = line();
    if (status == "fail") return start == bytes.size() && addresses.empty();
    if (status != "ok") return false;
    const auto count = line();
    if (count.empty() || count.size() > 2 ||
        !std::all_of(count.begin(), count.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const auto total = (count[0] - '0') + (count.size() == 2 ? 10 * (count[1] - '0') : 0);
    if (total < 1 || total > static_cast<int>(max_addresses)) return false;
    in_addr parsed{};
    for (int index = 0; index < total; ++index) {
        const auto address = line();
        if (address.empty() || address.size() > 15 ||
            !std::all_of(address.begin(), address.end(), [](char c) {
                return (c >= '0' && c <= '9') || c == '.';
            }) || ::inet_pton(AF_INET, std::string(address).c_str(), &parsed) != 1)
            return false;
        for (const auto& seen : addresses)
            if (seen == address) return false;
        addresses.emplace_back(address);
    }
    return start == bytes.size();
}

// Fixed argv/env are fully prepared before fork; the child only makes
// async-signal-safe calls before execveat on the pinned descriptor.
pid_t spawn_child(const PinnedHelper& helper, const std::string& hostname,
                  const std::string& service, int pipe_write) {
    std::array<char*, 4> argv{const_cast<char*>(helper_name),
        const_cast<char*>(hostname.c_str()), const_cast<char*>(service.c_str()), nullptr};
    std::array<char*, 1> envp{nullptr};
    const auto parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) throw Error(ErrorCode::Storage, "resolver helper fork failed");
    if (pid == 0) {
        struct rlimit core{0, 0};
        bool ready = ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0 &&
            ::getppid() == parent &&
            ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
            ::setrlimit(RLIMIT_CORE, &core) == 0;
        for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1,
                                 SIGUSR2, SIGCHLD, SIGWINCH, SIGALRM}) {
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            ready = ::sigaction(number, &action, nullptr) == 0 && ready;
        }
        const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        ready = null_fd >= 0 && ::dup2(null_fd, STDIN_FILENO) == 0 &&
            ::dup2(null_fd, STDERR_FILENO) == 0 && ::dup2(pipe_write, STDOUT_FILENO) == 0;
        ready = ready && ::close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0;
        if (!ready) ::_exit(127);
        char* const* child_argv = argv.data();
        char* const* child_envp = envp.data();
        ::execveat(helper.exec_fd, "", child_argv, child_envp, AT_EMPTY_PATH);
        ::_exit(127);
    }
    return pid;
}

enum class Stream { Continue, Eof, Failed };

Stream read_chunk(int fd, std::string& buffer) noexcept {
    std::array<char, 512> chunk{};
    const auto size = ::read(fd, chunk.data(), chunk.size());
    if (size > 0) {
        if (buffer.size() + static_cast<std::size_t>(size) > max_output_bytes)
            return Stream::Failed;
        buffer.append(chunk.data(), static_cast<std::size_t>(size));
        return Stream::Continue;
    }
    if (size == 0) return Stream::Eof;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return Stream::Continue;
    return Stream::Failed;
}

} // namespace

bool cleanup_pending() noexcept {
    const std::scoped_lock lock(slot_mutex);
    if (slot.pid == -1) return false;
    kill_child(slot.pid, slot.pidfd);
    if (reap_bounded(slot.pid, slot.pidfd,
                     Clock::now() + std::chrono::milliseconds(cleanup_window_ms))) {
        if (slot.pidfd >= 0) ::close(slot.pidfd);
        slot = PendingChild{};
        return false;
    }
    return true;
}

Observation resolve_ipv4(const std::string& hostname, const std::string& service,
                         Clock::time_point deadline, std::stop_token stop) {
    require(hostname.size() >= 1 && hostname.size() <= 253 && service.size() >= 1 &&
            service.size() <= 5 && hostname.find_first_of("/\\ \t\r\n") == std::string::npos &&
            hostname.find('\0') == std::string::npos &&
            std::all_of(service.begin(), service.end(), [](char c) {
                return c >= '0' && c <= '9';
            }), ErrorCode::InvalidInput, "resolver input must be one frozen hostname and port");
    // Precancel returns before any child is spawned or descriptor opened.
    if (stop.stop_requested()) return observation;
    {
        const std::scoped_lock lock(slot_mutex);
        if (slot.pid != -1) {
            if (reap_bounded(slot.pid, slot.pidfd, Clock::now() + std::chrono::milliseconds(1)))
                slot = PendingChild{};
            else
                throw Error(ErrorCode::Unsupported,
                            "a previous resolver child is still unreaped; live transport stays blocked");
        }
    }
    const auto helper = pin_helper();
    int descriptors[2] = {-1, -1};
    require(::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0, ErrorCode::Storage,
            "resolver helper pipe allocation failed");
    const auto started = Clock::now();
    Observation observation;
    observation.helper_identity = helper.identity;
    pid_t pid = -1;
    int pidfd = -1;
    bool cancelled = false;
    bool expired = false;
    bool failed = false;
    {
        // One in-flight child per process for the whole bounded lifetime.
        static std::mutex owner;
        const std::unique_lock<std::mutex> owned(owner, std::try_to_lock);
        if (!owned.owns_lock()) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            throw Error(ErrorCode::Unsupported,
                        "another resolver child is already in flight in this process");
        }
        pid = spawn_child(helper, hostname, service, descriptors[1]);
        // The child owns its duplicated write end; drop the parent copy now.
        ::close(descriptors[1]);
        descriptors[1] = -1;
        observation.started = true;
        std::string buffer;
        bool eof = false;
        while (!eof) {
            if (stop.stop_requested()) { cancelled = true; break; }
            if (Clock::now() >= deadline) { expired = true; break; }
            const auto step = read_chunk(descriptors[0], buffer);
            if (step == Stream::Failed) { failed = true; break; }
            if (step == Stream::Eof) { eof = true; break; }
            if (step == Stream::Continue) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    deadline - Clock::now()).count();
                pollfd descriptor{static_cast<short>(descriptors[0]), static_cast<short>(POLLIN), 0};
                (void)::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(0,
                    std::min<std::int64_t>(poll_slice_ms, remaining))));
            }
        }
        if (!cancelled && !expired && !failed && eof) {
            // EOF alone is not completion: the child must also be reaped with
            // a zero exit inside the remaining deadline budget.
            int status = 0;
            const auto until = std::min(deadline, Clock::now() +
                std::chrono::milliseconds(cleanup_window_ms));
            while (Clock::now() <= until) {
                const auto reaped = ::waitpid(pid, &status, WNOHANG);
                if (reaped == pid) {
                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                        observation.ok = parse_response(buffer, observation.addresses);
                    else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL &&
                             stop.stop_requested())
                        cancelled = true;
                    else
                        failed = true;
                    pid = -1;
                    break;
                }
                if (reaped != 0) { failed = true; pid = -1; break; }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
    if (pid != -1) {
        // Cancel, deadline, stream failure or an unreaped exit path: kill via
        // pidfd when possible and give the child one bounded cleanup window.
        kill_child(pid, pidfd);
        const auto until = std::min(deadline, Clock::now() +
            std::chrono::milliseconds(cleanup_window_ms));
        if (!reap_bounded(pid, pidfd, until)) {
            observation.cleanup_pending = true;
            const std::scoped_lock lock(slot_mutex);
            slot = PendingChild{pid, pidfd};
            pidfd = -1; // Ownership moved into the pending slot.
        }
    }
    if (pidfd >= 0) ::close(pidfd);
    ::close(descriptors[0]);
    observation.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started);
    (void)cancelled; (void)expired; (void)failed; // Transport maps ok=false to host reasons.
    return observation;
}

} // namespace wls::s1::resolver
