// Parent-side bounded lifecycle for the pinned wls861-resolve-ipv4 helper.
// Linux-only. Discovery is executable-relative only; each component is pinned
// with O_PATH|O_NOFOLLOW and every ancestor plus the helper leaf must be
// owner-pinned without group or world write — the sticky bit is not a
// substitute. Descriptors live in move-only owners whose moves transfer and
// invalidate the donor, so correctness never depends on elision. The parent
// reads a bounded nonblocking pipe under the caller's single original
// deadline — every wait is the exact remaining budget, never refreshed — and
// settles through one lock-free slot (idle -> active -> reaping -> idle or
// pending); an unreaped child parks pending and blocks later live resolves.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "https_resolver_internal.hpp"

#ifdef WLS_RESOLVER_TEST_SEAMS
#include "https_resolver_seams.hpp"
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/close_range.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>

#ifdef WLS_RESOLVER_TEST_SEAMS
#define WLS_RESOLVER_ALLOCATION_GUARD() ::wls::s1::resolver::seams::alloc_guard()
#else
#define WLS_RESOLVER_ALLOCATION_GUARD() (void)0
#endif

namespace wls::s1::resolver {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char helper_name[] = "wls861-resolve-ipv4";
constexpr std::int64_t poll_slice_ns = 25'000'000;

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}

// Move-only descriptor owner: a move transfers the descriptor and invalidates
// the donor, so returned values stay correct with or without copy elision;
// reset() takes a fresh descriptor, release() hands it to another owner.
struct UniqueFd {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int value) noexcept : fd(value) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    void reset(int value = -1) noexcept {
        if (fd >= 0) ::close(fd);
        fd = value;
    }
    int release() noexcept {
        const int value = fd;
        fd = -1;
        return value;
    }
    int get() const noexcept { return fd; }
};

enum class SlotState { Idle, Active, Pending, Reaping };

// Process-wide slot: one owner covers idle -> active -> reaping -> confirmed
// (idle again) or pending. Everything is one lock-free atomic state plus a
// pending payload that only the thread which CASes pending -> reaping may
// touch: payload writes happen before the pending publish and reads after the
// reaping claim. No mutex is ever held across a wait or syscall.
std::atomic<SlotState> slot_state{SlotState::Idle};
pid_t pending_pid{-1};
UniqueFd pending_pidfd;
static_assert(std::atomic<SlotState>::is_always_lock_free,
              "the resolver slot state must be lock-free on this target");

// The pinned helper: the execution descriptor is owned from the moment it is
// opened, so any later identity or ELF refusal unwinds through the owner.
struct PinnedHelper {
    UniqueFd exec_fd;
    std::string identity;
};

std::string self_executable() {
    std::array<char, 4096> buffer{};
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    // A result that fills the buffer may be a silently truncated path; refuse
    // before any pin, open or spawn instead of using the wrong object.
    require(size > 0 && static_cast<std::size_t>(size) < buffer.size() - 1,
            ErrorCode::Unsupported,
            "resolver helper discovery requires an untruncated /proc/self/exe path");
    return std::string(buffer.data(), static_cast<std::size_t>(size));
}

// Every ancestor up to and including the executable directory must be a
// directory owned by root or the effective user with neither group nor world
// write; the sticky bit does not turn a shared-write ancestor into an
// owner-pinned one.
void require_pinned_ancestor(int fd) {
    struct stat info {};
    require(::fstat(fd, &info) == 0, ErrorCode::Unsupported,
            "the resolver helper ancestry could not be observed");
    require(S_ISDIR(info.st_mode) &&
            (info.st_uid == 0 || info.st_uid == ::geteuid()) &&
            (info.st_mode & (S_IWGRP | S_IWOTH)) == 0,
            ErrorCode::Unsupported,
            "resolver helper ancestry must be owner-pinned without shared writes");
}

// Walks each component below / with O_PATH|O_NOFOLLOW, then pins the helper
// leaf. Symlinks, special files, wrong ownership, permissive modes or a
// truncation-length executable path are refused before any byte of the helper
// is read or executed.
PinnedHelper pin_helper() {
    const auto executable = self_executable();
    const auto slash = executable.find_last_of('/');
    require(slash != std::string::npos && slash != 0, ErrorCode::Unsupported,
            "resolver helper discovery found no executable directory");
    const auto directory = executable.substr(0, slash);
    UniqueFd dirfd{::open("/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    require(dirfd.get() >= 0, ErrorCode::Unsupported, "resolver helper root could not be pinned");
    require_pinned_ancestor(dirfd.get());
    std::size_t start = 1;
    while (start < directory.size()) {
        const auto end = directory.find('/', start);
        const auto component = directory.substr(start, (end == std::string::npos ?
            directory.size() : end) - start);
        const int next = ::openat(dirfd.get(), component.c_str(),
            O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(next >= 0, ErrorCode::Unsupported,
                "resolver helper directory could not be pinned without symlink descent");
        dirfd.reset(next); // the predecessor closes only after its successor exists
        require_pinned_ancestor(dirfd.get());
        start = end == std::string::npos ? directory.size() : end + 1;
    }
    UniqueFd path_fd{::openat(dirfd.get(), helper_name, O_PATH | O_NOFOLLOW | O_CLOEXEC)};
    require(path_fd.get() >= 0, ErrorCode::Unsupported,
            "the pinned resolver helper is missing next to the executable");
    struct stat pinned {};
    require(::fstat(path_fd.get(), &pinned) == 0, ErrorCode::Unsupported,
            "the pinned resolver helper could not be observed");
    require(S_ISREG(pinned.st_mode) &&
            (pinned.st_uid == 0 || pinned.st_uid == ::geteuid()) &&
            (pinned.st_mode & (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) == 0,
            ErrorCode::Unsupported,
            "the resolver helper must be a regular non-setuid file without shared writes");
#ifdef WLS_RESOLVER_TEST_SEAMS
    seams::pause_before_reopen();
#endif
    PinnedHelper helper;
    // The execution descriptor is owned from the moment it exists: any later
    // identity or ELF refusal unwinds through the RAII owner, never a leak.
    helper.exec_fd.reset(::openat(dirfd.get(), helper_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    require(helper.exec_fd.get() >= 0, ErrorCode::Unsupported,
            "the pinned resolver helper could not be opened for execution");
    struct stat opened {};
    require(::fstat(helper.exec_fd.get(), &opened) == 0 && opened.st_dev == pinned.st_dev &&
            opened.st_ino == pinned.st_ino && opened.st_mode == pinned.st_mode &&
            opened.st_uid == pinned.st_uid,
            ErrorCode::Unsupported, "the resolver helper changed while being pinned");
    std::array<char, 4> elf{};
    require(::pread(helper.exec_fd.get(), elf.data(), elf.size(), 0) == 4 && elf[0] == '\x7f' &&
            elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F',
            ErrorCode::Unsupported, "the resolver helper is not an ELF executable");
    WLS_RESOLVER_ALLOCATION_GUARD();
    // Private evidence only: the pinned object's real identity, including the
    // device and inode the pinned descriptors actually reference.
    helper.identity = directory + "/" + helper_name + "|bytes=" + std::to_string(pinned.st_size) +
        "|mode=0" + std::to_string(pinned.st_mode & 0777777) +
        "|dev=" + std::to_string(pinned.st_dev) +
        "|ino=" + std::to_string(pinned.st_ino);
    return helper;
}

// pidfd-first kill of this process's own child; the pidfd is opened on first
// use and falls back to the owned pid when the kernel (or a test refusal)
// will not provide one.
void kill_child(pid_t pid, UniqueFd& pidfd) noexcept {
    if (pidfd.get() < 0) {
#ifdef WLS_RESOLVER_TEST_SEAMS
        pidfd.reset(seams::open_pidfd_observed(pid));
#elif defined(SYS_pidfd_open)
        pidfd.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
#endif
    }
    bool sent = false;
#ifdef SYS_pidfd_send_signal
    if (pidfd.get() >= 0)
        sent = ::syscall(SYS_pidfd_send_signal, pidfd.get(), SIGKILL, nullptr, 0) == 0;
#endif
    if (!sent) (void)::kill(pid, SIGKILL);
}

// Reap and wait observation funnels: production builds call the real syscalls
// directly; the test-special compilation routes through the seam header,
// which can hold back one nonblocking confirmation (deterministic pending),
// inject repeated EINTR or an unknown waitpid error, and records the
// bounded-wait facts the orchestrator asserts over — each wait's real now,
// the one original deadline, the requested timeout and the phase identity.
pid_t observe_waitpid(pid_t pid, int* status, int options,
                      [[maybe_unused]] bool admission = false) noexcept {
#ifdef WLS_RESOLVER_TEST_SEAMS
    return seams::waitpid_observed(pid, status, options, admission);
#else
    return ::waitpid(pid, status, options);
#endif
}

int observe_ppoll(pollfd* descriptors, nfds_t count, std::int64_t requested_ns,
                  [[maybe_unused]] std::int64_t deadline_ns,
                  [[maybe_unused]] std::int64_t now_ns, [[maybe_unused]] bool reap) noexcept {
#ifdef WLS_RESOLVER_TEST_SEAMS
    seams::record_wait(deadline_ns, now_ns, requested_ns, reap);
#endif
    const timespec pause{static_cast<time_t>(requested_ns / 1'000'000'000),
                         static_cast<long>(requested_ns % 1'000'000'000)};
    return ::ppoll(descriptors, count, &pause, nullptr);
}

// The one bounded reap loop used by every confirm path (clean EOF, cancel and
// unwind settle). True means a real waitpid confirmed the exit and filled
// status; an unknown waitpid error is never exit proof. Every iteration —
// EINTR retries included — is bounded by the same caller deadline, each wait
// is the exact remaining budget truncated to the poll slice, and an absent
// pidfd degrades to a bounded zero-descriptor wait, never an unrelated fd.
bool reap_bounded(pid_t pid, UniqueFd& pidfd, Clock::time_point until,
                  int& status) noexcept {
    for (;;) {
        const pid_t observed = observe_waitpid(pid, &status, WNOHANG);
        if (observed == pid) return true;
        if (observed != 0 && observed != pid &&
            (observed != -1 || errno != EINTR))
            return false; // an unknown result is never exit proof
        const auto now = Clock::now();
        if (now >= until) return false;
        const auto requested = std::min<std::int64_t>(poll_slice_ns,
            std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count());
        pollfd descriptor{pidfd.get(), static_cast<short>(POLLIN), 0};
        (void)observe_ppoll(descriptor.fd >= 0 ? &descriptor : nullptr,
                            static_cast<nfds_t>(descriptor.fd >= 0 ? 1 : 0), requested,
                            until.time_since_epoch().count(), now.time_since_epoch().count(), true);
    }
}

// One nonblocking pending confirmation under the reaping claim: a really
// observed exit closes the pending pidfd and publishes Idle; anything else
// publishes Pending back.
bool confirm_pending_once() noexcept {
    int status = 0;
    if (observe_waitpid(pending_pid, &status, WNOHANG, true) == pending_pid) {
        pending_pidfd.reset();
        pending_pid = -1;
        slot_state.store(SlotState::Idle, std::memory_order_release);
        return true;
    }
    slot_state.store(SlotState::Pending, std::memory_order_release);
    return false;
}

// Move-only owner of one forked child and this call's pipe ends. `armed`
// marks the process-wide Active slot this object owns; it is cleared exactly
// once — on the release to Idle or the handover to Pending — so a late
// destructor can never touch a successor's slot. The noexcept destructor
// never allocates; a throwing observation unwinds through it and still
// settles the child.
struct ChildOwnership {
    Clock::time_point deadline{};
    bool armed{false};
    bool owns_child{false};
    pid_t pid{-1};
    UniqueFd pidfd;
    UniqueFd read_fd;
    UniqueFd write_fd; // held until the fork outcome closes the parent copy
    ChildOwnership() = default;
    ChildOwnership(const ChildOwnership&) = delete;
    ChildOwnership& operator=(const ChildOwnership&) = delete;
    ~ChildOwnership() { (void)settle(); }

    // Bounded CAS admission: idle -> active makes this object the sole owner.
    // A pending predecessor gets exactly one nonblocking confirmation — never
    // a poll or a fresh wait budget; active/reaping are refused without
    // spinning. Throws before any descriptor is opened.
    void claim(Clock::time_point limit) {
        deadline = limit;
        for (int attempts = 0; attempts < 4; ++attempts) {
            SlotState observed = slot_state.load(std::memory_order_acquire);
            if (observed == SlotState::Pending) {
                if (slot_state.compare_exchange_strong(observed, SlotState::Reaping,
                                                        std::memory_order_acq_rel) &&
                    !confirm_pending_once())
                    throw Error(ErrorCode::Unsupported,
                                "a previous resolver child is still unreaped; live transport stays blocked");
            } else if (observed == SlotState::Idle &&
                       slot_state.compare_exchange_strong(observed, SlotState::Active,
                                                          std::memory_order_acq_rel)) {
                armed = true;
                return;
            } else {
                throw Error(ErrorCode::Unsupported,
                            "another resolver child is already in flight in this process");
            }
        }
        throw Error(ErrorCode::Unsupported, "resolver slot admission changed under way");
    }

    // A real reap already confirmed the exit: drop the child facts, close the
    // pidfd, then publish Idle — exactly once per armed owner.
    void confirm_exit() noexcept {
        pidfd.reset();
        pid = -1;
        owns_child = false;
        if (armed) {
            armed = false;
            slot_state.store(SlotState::Idle, std::memory_order_release);
        }
    }

    // Terminal settle for an owned child: kill, then one bounded reap window
    // that the original deadline still bounds. A confirmed exit releases the
    // slot to Idle; an unconfirmed child hands its pid/pidfd to the pending
    // payload (written before the Pending publish) and stays pending.
    bool settle() noexcept {
        if (!armed) return false;
        if (!owns_child) {
            armed = false;
            slot_state.store(SlotState::Idle, std::memory_order_release);
            return false;
        }
        kill_child(pid, pidfd);
        int status = 0;
        const auto until = std::min(deadline, Clock::now() +
            std::chrono::nanoseconds(poll_slice_ns));
        if (reap_bounded(pid, pidfd, until, status)) {
            confirm_exit();
            return false;
        }
        pending_pid = pid;
        pending_pidfd.reset(pidfd.release());
        pid = -1;
        owns_child = false;
        armed = false;
        slot_state.store(SlotState::Pending, std::memory_order_release);
        return true;
    }
};

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
    if (status == "fail") return false;
    if (status != "ok") return false;
    const auto count = line();
    if (count.empty() || count.size() > 2 ||
        !std::all_of(count.begin(), count.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    if (count.size() == 2 && count[0] == '0') return false;
    const auto total = count.size() == 2 ? 10 * (count[0] - '0') + (count[1] - '0')
                                         : count[0] - '0';
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
        WLS_RESOLVER_ALLOCATION_GUARD();
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
#ifdef WLS_RESOLVER_TEST_SEAMS
    const pid_t pid = seams::fork_observed();
#else
    const pid_t pid = ::fork();
#endif
    if (pid < 0) throw Error(ErrorCode::Storage, "resolver helper fork failed");
    if (pid == 0) {
        struct rlimit core{0, 0};
        bool ready = ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0;
        ready = ::getppid() == parent && ready;
        ready = ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && ready;
        ready = ::setrlimit(RLIMIT_CORE, &core) == 0 && ready;
        for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1,
                                 SIGUSR2, SIGCHLD, SIGWINCH, SIGALRM}) {
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            ready = ::sigaction(number, &action, nullptr) == 0 && ready;
        }
        sigset_t empty_mask;
        ::sigemptyset(&empty_mask);
        ready = ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr) == 0 && ready;
        const int exec_source = ::fcntl(helper.exec_fd.get(), F_DUPFD_CLOEXEC, 3);
        const int output_source = ::fcntl(pipe_write, F_DUPFD_CLOEXEC, 3);
        const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        const int input_source = null_fd < 0 ? -1 : ::fcntl(null_fd, F_DUPFD_CLOEXEC, 3);
        ready = exec_source >= 3 && output_source >= 3 && input_source >= 3 && ready;
        if (ready) {
            ready = ::dup2(input_source, STDIN_FILENO) == STDIN_FILENO && ready;
            ready = ::dup2(input_source, STDERR_FILENO) == STDERR_FILENO && ready;
            ready = ::dup2(output_source, STDOUT_FILENO) == STDOUT_FILENO && ready;
        }
        ready = ::close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0 && ready;
        if (!ready) ::_exit(127);
        ::execveat(exec_source, "", argv.data(), envp.data(), AT_EMPTY_PATH);
        ::_exit(127);
    }
    return pid;
}

enum class Stream { Continue, Eof, Failed };

// May throw (bounded append): the exception propagates to the caller and the
// move-only owner settles the child during unwinding. A noexcept wrapper that
// could terminate the host on allocation failure is not allowed here.
Stream read_chunk(int fd, std::string& buffer) {
    std::array<char, 512> chunk{};
#ifdef WLS_RESOLVER_TEST_SEAMS
    const auto size = seams::read_observed(fd, chunk.data(), chunk.size());
#else
    const auto size = ::read(fd, chunk.data(), chunk.size());
#endif
    if (size > 0) {
        if (buffer.size() + static_cast<std::size_t>(size) > max_output_bytes)
            return Stream::Failed;
        WLS_RESOLVER_ALLOCATION_GUARD();
        buffer.append(chunk.data(), static_cast<std::size_t>(size));
        return Stream::Continue;
    }
    if (size == 0) return Stream::Eof;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return Stream::Continue;
    return Stream::Failed;
}

} // namespace

bool cleanup_pending() noexcept {
    // Single non-blocking observation, never a fresh wait budget: true while
    // any child is still owned (pending-unobserved or active/reaping); false
    // only when the slot is Idle. Never waits, never kills — the parked child
    // already received its SIGKILL and a later kill could hit a reused pid.
    SlotState observed = slot_state.load(std::memory_order_acquire);
    if (observed == SlotState::Idle) return false;
    if (observed != SlotState::Pending) return true;
    if (!slot_state.compare_exchange_strong(observed, SlotState::Reaping,
                                            std::memory_order_acq_rel))
        return true;
    return !confirm_pending_once();
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
    if (stop.stop_requested()) return {};
    ChildOwnership child;
    child.claim(deadline);
    const auto helper = pin_helper();
    int descriptors[2] = {-1, -1};
#ifdef WLS_RESOLVER_TEST_SEAMS
    require(seams::pipe2_observed(descriptors, O_CLOEXEC | O_NONBLOCK) == 0,
            ErrorCode::Storage, "resolver helper pipe allocation failed");
#else
    require(::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0, ErrorCode::Storage,
            "resolver helper pipe allocation failed");
#endif
    child.read_fd.reset(descriptors[0]);
    child.write_fd.reset(descriptors[1]);
    const auto started = Clock::now();
    Observation observation;
    WLS_RESOLVER_ALLOCATION_GUARD();
    observation.helper_identity = helper.identity;
    // Stop and deadline are re-checked immediately before the fork.
    if (stop.stop_requested() || Clock::now() >= deadline) {
        child.write_fd.reset();
        if (stop.stop_requested()) return {};
    } else {
        child.pid = spawn_child(helper, hostname, service, child.write_fd.get());
        child.owns_child = true;
        child.write_fd.reset();
        observation.started = true;
    }
    if (child.owns_child) {
        std::string buffer;
        bool eof = false;
        while (!eof) {
            if (stop.stop_requested()) break;
            if (Clock::now() >= deadline) break;
            const auto step = read_chunk(child.read_fd.get(), buffer);
            if (step == Stream::Failed) break;
            if (step == Stream::Eof) { eof = true; break; }
            const auto now = Clock::now();
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - now).count();
            if (remaining <= 0) continue; // never issue a wait on a spent budget
            pollfd descriptor{child.read_fd.get(), static_cast<short>(POLLIN), 0};
            (void)observe_ppoll(&descriptor, 1,
                std::min<std::int64_t>(poll_slice_ns, remaining),
                deadline.time_since_epoch().count(), now.time_since_epoch().count(), false);
        }
        if (eof) {
            int status = 0;
            const auto confirm_by = std::min(deadline, Clock::now() +
                std::chrono::nanoseconds(poll_slice_ns));
            if (reap_bounded(child.pid, child.pidfd, confirm_by, status)) {
                const bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                // The confirmed exit is registered and the slot released
                // before any throwing bookkeeping such as the response
                // allocations; a visible stop never adopts a normal result.
                child.confirm_exit();
#ifdef WLS_RESOLVER_TEST_SEAMS
                seams::pause_after_release();
#endif
                if (clean_exit && !stop.stop_requested()) {
                    // Final adoption gate: parsing and its allocations may
                    // cross the original deadline, so eligibility is re-proven
                    // here — an expired confirm refuses the result, never the
                    // real reap, and no adoptable address survives.
                    observation.ok = parse_response(buffer, observation.addresses) &&
                        !stop.stop_requested() && Clock::now() < deadline;
                    if (!observation.ok) observation.addresses.clear();
                }
            }
        }
        if (child.owns_child)
            observation.cleanup_pending = child.settle();
    }
    observation.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started);
    return observation;
}

} // namespace wls::s1::resolver
