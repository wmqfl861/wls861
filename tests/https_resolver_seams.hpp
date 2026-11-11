#pragma once

// Test-special compilation seams for the resolver lifecycle. The state and
// helpers below are shared between src/https_resolver_posix.cpp compiled with
// WLS_RESOLVER_TEST_SEAMS (only ever linked into https_resolver_tests) and
// the staged scenarios of the same driver binary. The production library
// object, the wls/wls861 executables and the wls861-resolve-ipv4 helper never
// see this header, and no production CLI or API switch exists: arming a seam
// is a property of this process's own throwaway fixture children and staged
// copies only. Every hook degrades to the real syscall observation when
// unarmed; counters and wait facts are passive bounded facts for the
// orchestrator's assertions — no throwing allocation inside a noexcept hook.
// Wait facts are validated at record time against the frozen original
// deadline and the phase-aware single-deadline contract (read facts carry the
// original exactly, reap facts may be stricter, legacy records without a
// production-captured now identity are rejected); violations latch stickily,
// so a bounded ring overwrite can never hide an early violation.
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <limits>
#include <new>
#include <type_traits>

namespace wls::s1::resolver::seams {

// Only the subms staged scenario arms this POD on its calling thread. The
// executable's strong clock_gettime symbol is visible to loaded libraries as
// well: TLS/PID constrain returned controlled values, not symbol visibility.
// Every call first executes the real syscall, preserving libc failure/errno
// behavior; unarmed, other-thread, fork-child and non-MONOTONIC calls keep it.
struct ControlledClock {
    pid_t owner_pid;
    bool armed;
    bool monotonic;
    bool first_fact_valid;
    int phase; // 0 preparation, 1 consumed read EINTR, 2 recorded first read wait
    int hits;
    int read_eintr;
    int read_facts;
    long long base_ns;
    long long deadline_ns;
    long long now_ns;
};
static_assert(std::is_trivial_v<ControlledClock>);
inline thread_local ControlledClock controlled_clock{};

inline bool ns_to_timespec(long long ns, timespec& value) noexcept {
    if (ns < 0 || static_cast<unsigned long long>(ns / 1'000'000'000LL) >
        static_cast<unsigned long long>(std::numeric_limits<time_t>::max()))
        return false;
    value.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
    value.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
    return true;
}

inline bool timespec_to_ns(const timespec& value, long long& ns) noexcept {
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L ||
        static_cast<unsigned long long>(value.tv_sec) >
        static_cast<unsigned long long>((std::numeric_limits<long long>::max() -
            value.tv_nsec) / 1'000'000'000LL))
        return false;
    ns = static_cast<long long>(value.tv_sec) * 1'000'000'000LL + value.tv_nsec;
    return true;
}

inline bool real_clock_ns(clockid_t clock, long long& ns) noexcept {
    timespec value{};
    return ::syscall(SYS_clock_gettime, clock, &value) == 0 && timespec_to_ns(value, ns);
}

inline bool controlled_clock_active() noexcept {
    return controlled_clock.armed && controlled_clock.owner_pid == ::getpid();
}

inline int clock_gettime_observed(clockid_t clock, timespec* value) noexcept {
    const int result = static_cast<int>(::syscall(SYS_clock_gettime, clock, value));
    if (result != 0 || clock != CLOCK_MONOTONIC || !controlled_clock_active())
        return result;
    timespec controlled{};
    if (!ns_to_timespec(controlled_clock.now_ns, controlled)) {
        errno = EOVERFLOW;
        return -1;
    }
    *value = controlled;
    ++controlled_clock.hits;
    return 0;
}

class ScopedSubmsClock {
    bool engaged_ = false;
public:
    ScopedSubmsClock(long long base_ns, long long deadline_ns) noexcept {
        timespec checked{};
        if (controlled_clock.armed || base_ns <= 0 ||
            base_ns > std::numeric_limits<long long>::max() - 700'000 ||
            deadline_ns != base_ns + 700'000 || !ns_to_timespec(deadline_ns, checked))
            return;
        controlled_clock = {::getpid(), true, true, false, 0, 0, 0, 0,
                            base_ns, deadline_ns, base_ns};
        engaged_ = true;
    }
    ~ScopedSubmsClock() { if (engaged_) controlled_clock.armed = false; }
    bool engaged() const noexcept { return engaged_; }
    ScopedSubmsClock(const ScopedSubmsClock&) = delete;
    ScopedSubmsClock& operator=(const ScopedSubmsClock&) = delete;
};

inline void clock_after_read_eintr() noexcept {
    if (!controlled_clock_active()) return;
    auto& clock = controlled_clock;
    ++clock.read_eintr;
    if (clock.phase != 0 || clock.now_ns != clock.base_ns) {
        clock.monotonic = false;
        clock.armed = false;
        return;
    }
    clock.now_ns = clock.base_ns + 100'000;
    clock.phase = 1;
}

// Called only AFTER the passive recorder validates and stores the actual
// production arguments. A bad fact remains sticky; it disarms the clock so
// even a sensitivity mutation cannot freeze a failing resolver indefinitely.
inline void clock_after_read_fact(long long cutoff_ns, long long now_ns,
                                 long long requested_ns, int phase,
                                 bool violation) noexcept {
    if (!controlled_clock_active() || phase != 1) return;
    auto& clock = controlled_clock;
    ++clock.read_facts;
    clock.first_fact_valid = !violation && clock.phase == 1 &&
        clock.now_ns == clock.base_ns + 100'000 && now_ns == clock.now_ns &&
        cutoff_ns == clock.deadline_ns && requested_ns == 600'000;
    if (!clock.first_fact_valid) {
        clock.monotonic = false;
        clock.armed = false;
        return;
    }
    clock.now_ns = clock.deadline_ns;
    clock.phase = 2;
}

struct State {
    // Deterministic pending: while armed, nonblocking reap observation of the
    // owned child reports "not exited yet" without consuming the exit status,
    // so the pending state machine is proven without guessing kill timing.
    std::atomic_bool hold_reap{false};
    // Barrier hook: pause the owner right after it released the slot so the
    // orchestrator can start a successor before this object's destructor runs.
    std::atomic_bool hold_after_release{false};
    std::atomic_int release_seen{0};
    // Test-only explicit ready/release barrier after leaf type checks, before
    // the pathname reopen. Unarmed production-seam tests pass straight through.
    std::atomic_bool hold_before_reopen{false};
    std::atomic_int reopen_seen{0};
    // Fault injections. The waitpid counters are consumed per call (a large
    // count models continuous EINTR or repeated unknown errors); the pidfd
    // refusal is a mode; the allocation guard throws at the Nth guarded site.
    std::atomic_int inject_waitpid_eintr{0};
    std::atomic_int inject_waitpid_unknown{0};
    std::atomic_int inject_fork_failure{0};
    std::atomic_int inject_pipe_failure{0};
    std::atomic_int inject_read_eintr{0};
    std::atomic_int read_eintr_consumed{0};
    std::atomic_bool inject_pidfd_refusal{false};
    std::atomic_int inject_alloc_failure{0};
    // Ownership observations: real forks of this process, reap attempts and
    // confirmations, admission confirmations (never allowed to poll), the
    // bounded-wait facts of every wait (the original deadline, production's
    // captured now and requested timeout of the last fact_capacity waits),
    // and the legacy millisecond projections the C1 derived-RED build still asserts.
    // The subms scenario labels its controlled_test now/cutoff explicitly;
    // those facts prove production calculations, not real 700us performance.
    std::atomic_int fork_count{0};
    std::atomic_int waitpid_calls{0};
    std::atomic_int waitpid_confirmed{0};
    std::atomic_int admission_waitpid{0};
    std::atomic_int admission_polls{0};
    std::atomic_int poll_calls{0};
    std::atomic_int poll_max_timeout_ms{0};
    std::atomic_int poll_nonpositive_deadline{0};
    std::atomic_llong poll_max_requested_ns{0};
    static constexpr int fact_capacity = 32;
    struct WaitFact {
        std::atomic_llong now_ns{0};
        std::atomic_llong deadline_ns{0};
        std::atomic_llong requested_ns{0};
        std::atomic_int phase{0}; // 0 unknown/legacy, 1 read phase, 2 reap phase
    };
    WaitFact wait_facts[fact_capacity]{};
    std::atomic_int wait_fact_count{0};
    // Strict single-deadline oracle: the scenario freezes its independently
    // captured original deadline before the resolve; every recorded wait is
    // validated at record time and any violation latches here stickily until
    // the scenario's own reset. Totals stay monotonic, so a bounded ring
    // overwrite can never hide an early violation (completeness rests on the
    // per-record latch, not on the surviving window).
    std::atomic_bool original_deadline_frozen{false};
    std::atomic_llong frozen_original_deadline_ns{0};
    std::atomic_int wait_fact_violations{0};
    std::atomic_bool wait_fact_violation_latched{false};
    std::atomic_bool wait_fact_overflow{false};
    std::atomic_int read_wait_facts{0};
    std::atomic_int reap_wait_facts{0};
    // Real identity: the scenario's first real fork and the pid a real
    // admission-flagged waitpid actually confirmed, so pending release is
    // bound to the original child instead of derived from booleans/counts.
    std::atomic_int first_forked_pid{-1};
    std::atomic_int admission_confirmed_pid{-1};
    // The pid a real waitpid confirmed most recently (admission or reap), so
    // adoptions and pending releases are bound to the original child identity;
    // and the D1 barrier, which pauses the first real confirm of the
    // scenario's own child before the waitpid happens — bounded test machinery
    // with its own outer safety limit, never a fresh wait budget for
    // production and never the admission path.
    std::atomic_int last_confirmed_pid{-1};
    std::atomic_bool hold_first_confirm{false};
    std::atomic_int confirm_gate_seen{0};
};

inline State state;

// Consumes one unit from a counter; true when a unit was available.
inline bool consume(std::atomic_int& counter) noexcept {
    int current = counter.load(std::memory_order_relaxed);
    while (current > 0 &&
           !counter.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
    return current > 0;
}

// Throws at the Nth guarded allocation site (N = inject_alloc_failure): each
// guard consumes one unit and the site that takes the last one throws.
inline void alloc_guard() {
    if (state.inject_alloc_failure.load(std::memory_order_relaxed) <= 0) return;
    if (state.inject_alloc_failure.fetch_sub(1, std::memory_order_relaxed) == 1)
        throw std::bad_alloc();
}

inline pid_t fork_observed() noexcept {
    pid_t pid = -1;
    if (!consume(state.inject_fork_failure))
        pid = ::fork();
    if (pid > 0) {
        state.fork_count.fetch_add(1, std::memory_order_relaxed);
        int unset = -1;
        state.first_forked_pid.compare_exchange_strong(unset, pid,
                                                       std::memory_order_relaxed);
    }
    return pid;
}

inline int pipe2_observed(int descriptors[2], int flags) noexcept {
    if (consume(state.inject_pipe_failure)) return -1;
    return ::pipe2(descriptors, flags);
}

inline ssize_t read_observed(int fd, void* buffer, std::size_t size) noexcept {
    if (consume(state.inject_read_eintr)) {
        state.read_eintr_consumed.fetch_add(1, std::memory_order_relaxed);
        clock_after_read_eintr();
        errno = EINTR; // One injected interruption: the retry path must hold.
        return -1;
    }
    return ::read(fd, buffer, size);
}

inline int open_pidfd_observed(pid_t pid) noexcept {
    if (state.inject_pidfd_refusal.load(std::memory_order_relaxed)) return -1;
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    return -1;
#endif
}

inline pid_t waitpid_observed(pid_t pid, int* status, int options,
                              bool admission) noexcept {
    state.waitpid_calls.fetch_add(1, std::memory_order_relaxed);
    if (admission)
        state.admission_waitpid.fetch_add(1, std::memory_order_relaxed);
    if (consume(state.inject_waitpid_eintr)) {
        errno = EINTR;
        return -1;
    }
    if (consume(state.inject_waitpid_unknown)) {
        errno = EINVAL;
        return -1;
    }
    if (state.hold_reap.load(std::memory_order_relaxed)) return 0;
    if (!admission && state.hold_first_confirm.load(std::memory_order_relaxed) &&
        pid == state.first_forked_pid.load(std::memory_order_relaxed)) {
        // D1 barrier: bound to the scenario's own child and the confirm phase;
        // the bounded spin is the barrier's own outer safety limit, separate
        // from any business deadline, and the real waitpid always follows.
        state.confirm_gate_seen.fetch_add(1, std::memory_order_relaxed);
        for (int spin = 0; spin < 30000 &&
             state.hold_first_confirm.load(std::memory_order_relaxed); ++spin)
            (void)::poll(nullptr, 0, 1);
    }
    const pid_t observed = ::waitpid(pid, status, options);
    if (observed == pid) {
        state.waitpid_confirmed.fetch_add(1, std::memory_order_relaxed);
        state.last_confirmed_pid.store(pid, std::memory_order_relaxed);
        if (admission)
            state.admission_confirmed_pid.store(pid, std::memory_order_relaxed);
    }
    return observed;
}

// Bounded wait facts for one ppoll/poll decision, validated at record time:
// the effective cutoff production adopted, the now production captured when
// it computed the request, the requested timeout and the phase identity.
// Validation happens before the ring slot is rewritten, so an early violation
// can never be overwritten into silence; the frozen original deadline bounds
// both phases (the read phase carries it exactly, the reap phase may be
// stricter). Also keeps the legacy millisecond projections (max requested ms,
// nonpositive-request canary) that the C1 derived-RED build of the frozen
// source asserts over.
inline void record_wait_detail(long long cutoff_ns, long long now_ns,
                               long long requested_ns, int phase) noexcept {
    state.poll_calls.fetch_add(1, std::memory_order_relaxed);
    if (phase == 1) state.read_wait_facts.fetch_add(1, std::memory_order_relaxed);
    if (phase == 2) state.reap_wait_facts.fetch_add(1, std::memory_order_relaxed);
    bool violation = now_ns <= 0 || requested_ns <= 0 || requested_ns > 25'000'000LL ||
        now_ns >= cutoff_ns || requested_ns > cutoff_ns - now_ns;
    if (!violation && state.original_deadline_frozen.load(std::memory_order_relaxed)) {
        const long long original =
            state.frozen_original_deadline_ns.load(std::memory_order_relaxed);
        // Phase-aware single-deadline contract: the read phase carries the
        // frozen original exactly; the reap phase may adopt the stricter
        // min(deadline, now+25ms) term but never anything later; a legacy
        // record without real now identity is rejected outright.
        if (original <= 0 || phase == 0 ||
            (phase == 1 ? cutoff_ns != original : cutoff_ns > original))
            violation = true;
    }
    if (violation) {
        state.wait_fact_violations.fetch_add(1, std::memory_order_relaxed);
        state.wait_fact_violation_latched.store(true, std::memory_order_relaxed);
    }
    if (requested_ns > state.poll_max_requested_ns.load(std::memory_order_relaxed))
        state.poll_max_requested_ns.store(requested_ns, std::memory_order_relaxed);
    const long long requested_ms = requested_ns / 1'000'000;
    if (requested_ms > state.poll_max_timeout_ms.load(std::memory_order_relaxed))
        state.poll_max_timeout_ms.store(static_cast<int>(requested_ms),
                                        std::memory_order_relaxed);
    // The canary is exact in nanoseconds: a sub-millisecond request (for
    // example the exact 700us remainder) is a positive, legal budget and must
    // not be counted by the legacy millisecond projection as nonpositive.
    if (requested_ns <= 0)
        state.poll_nonpositive_deadline.fetch_add(1, std::memory_order_relaxed);
    const int previous = state.wait_fact_count.fetch_add(1, std::memory_order_relaxed);
    if (previous >= State::fact_capacity)
        state.wait_fact_overflow.store(true, std::memory_order_relaxed);
    State::WaitFact& fact = state.wait_facts[previous % State::fact_capacity];
    fact.now_ns.store(now_ns, std::memory_order_relaxed);
    fact.deadline_ns.store(cutoff_ns, std::memory_order_relaxed);
    fact.requested_ns.store(requested_ns, std::memory_order_relaxed);
    fact.phase.store(phase, std::memory_order_relaxed);
    clock_after_read_fact(cutoff_ns, now_ns, requested_ns, phase, violation);
}

// Production entry: cutoff, now and phase are the production-captured values
// passed through the passive observation parameters of observe_ppoll.
inline void record_wait(long long cutoff_ns, long long now_ns, long long requested_ns,
                        bool reap_phase) noexcept {
    record_wait_detail(cutoff_ns, now_ns, requested_ns, reap_phase ? 2 : 1);
}

// Legacy two-argument entry kept for the frozen C1 derived-RED input, whose
// production predates the passive wiring: such records carry no captured now
// identity, so the strict oracle latches the gap instead of vacuously
// accepting the record.
inline void record_wait(long long deadline_ns, long long requested_ns) noexcept {
    record_wait_detail(deadline_ns, 0, requested_ns, 0);
}

// The scenario freezes its independently captured original deadline before
// the resolve; record-time validation latches any wait beyond it (and rejects
// a nonsensical non-positive frozen original outright).
inline void freeze_original_deadline(long long original_ns) noexcept {
    state.frozen_original_deadline_ns.store(original_ns, std::memory_order_relaxed);
    state.original_deadline_frozen.store(true, std::memory_order_relaxed);
}

// True when at least one read-phase fact is present and every one in the
// observable window carries the original deadline exactly. Violations beyond
// the original already latched at record time; this scan is the reporting
// projection for the read phase, not the guard itself.
inline bool read_facts_carry_original_exactly(long long original_ns) noexcept {
    const int total = state.wait_fact_count.load(std::memory_order_relaxed);
    const int window = total < State::fact_capacity ? total : State::fact_capacity;
    bool seen = false;
    for (int index = 0; index < window; ++index) {
        const State::WaitFact& fact = state.wait_facts[index];
        if (fact.phase.load(std::memory_order_relaxed) != 1) continue;
        seen = true;
        if (fact.deadline_ns.load(std::memory_order_relaxed) != original_ns)
            return false;
    }
    return seen;
}

// True only when a real admission-flagged waitpid confirmed the scenario's
// first real fork: both identities must exist — a pair of untouched -1
// sentinels comparing equal is not a binding.
inline bool pid_bind_proven() noexcept {
    const int first = state.first_forked_pid.load(std::memory_order_relaxed);
    return first > 0 && state.admission_confirmed_pid.load(std::memory_order_relaxed) == first;
}

// Barrier hook for the owner that just released the slot: bounded and
// non-allocating; these pauses are test machinery, not recorded wait facts.
inline void pause_after_release() noexcept {
    if (!state.hold_after_release.load(std::memory_order_relaxed)) return;
    state.release_seen.fetch_add(1, std::memory_order_relaxed);
    for (int spin = 0; spin < 10000 &&
         state.hold_after_release.load(std::memory_order_relaxed); ++spin)
        (void)::poll(nullptr, 0, 1);
}

inline void pause_before_reopen() noexcept {
    if (!state.hold_before_reopen.load(std::memory_order_acquire)) return;
    state.reopen_seen.fetch_add(1, std::memory_order_release);
    for (int spin = 0; spin < 10000 &&
         state.hold_before_reopen.load(std::memory_order_acquire); ++spin)
        (void)::poll(nullptr, 0, 1);
}

inline void reset() {
    state.hold_before_reopen.store(false);
    state.reopen_seen.store(0);
    state.hold_reap.store(false);
    state.hold_after_release.store(false);
    state.release_seen.store(0);
    state.inject_waitpid_eintr.store(0);
    state.inject_waitpid_unknown.store(0);
    state.inject_fork_failure.store(0);
    state.inject_pipe_failure.store(0);
    state.inject_read_eintr.store(0);
    state.read_eintr_consumed.store(0);
    state.inject_pidfd_refusal.store(false);
    state.inject_alloc_failure.store(0);
    state.fork_count.store(0);
    state.waitpid_calls.store(0);
    state.waitpid_confirmed.store(0);
    state.admission_waitpid.store(0);
    state.admission_polls.store(0);
    state.poll_calls.store(0);
    state.poll_max_timeout_ms.store(0);
    state.poll_nonpositive_deadline.store(0);
    state.poll_max_requested_ns.store(0);
    state.wait_fact_count.store(0);
    state.original_deadline_frozen.store(false);
    state.frozen_original_deadline_ns.store(0);
    state.wait_fact_violations.store(0);
    state.wait_fact_violation_latched.store(false);
    state.wait_fact_overflow.store(false);
    state.read_wait_facts.store(0);
    state.reap_wait_facts.store(0);
    state.first_forked_pid.store(-1);
    state.admission_confirmed_pid.store(-1);
    state.last_confirmed_pid.store(-1);
    state.hold_first_confirm.store(false);
    state.confirm_gate_seen.store(0);
    for (State::WaitFact& fact : state.wait_facts) {
        fact.now_ns.store(0);
        fact.deadline_ns.store(0);
        fact.requested_ns.store(0);
        fact.phase.store(0);
    }
}

} // namespace wls::s1::resolver::seams
