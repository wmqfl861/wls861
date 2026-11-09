// Private Linux clock adapter for the Controls W/D foundation. This is the
// only file with platform I/O: it reads the fixed kernel boot identity and
// the boot/realtime clocks in the frozen capture order. No environment
// variable, configuration key, or model input can redirect the path or alter
// the clocks; every failure is a typed unavailable result with no fallback.
#define _DEFAULT_SOURCE 1

#include "controls_detail.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace wls::controls::detail {
namespace {

constexpr char boot_id_path[] = "/proc/sys/kernel/random/boot_id";

// Fixed-layout read of the boot identity: exactly 36 layout characters plus
// a newline, then proven EOF. Anything longer, shorter, or differently
// formatted is a failure.
bool read_boot_id_fixed_format(char (&out)[boot_id_length + 1]) {
    const int fd = ::open(boot_id_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[boot_id_length + 2];
    ssize_t first = -1;
    do {
        first = ::read(fd, buffer, sizeof(buffer));
    } while (first < 0 && errno == EINTR);
    char extra = '\0';
    ssize_t second = -1;
    if (first >= 0) {
        do {
            second = ::read(fd, &extra, 1);
        } while (second < 0 && errno == EINTR);
    }
    ::close(fd);
    if (first != static_cast<ssize_t>(boot_id_length + 1)) return false;
    if (second != 0) return false;  // more content than the fixed layout
    if (buffer[boot_id_length] != '\n') return false;
    for (std::size_t i = 0; i < boot_id_length; ++i) out[i] = buffer[i];
    out[boot_id_length] = '\0';
    return boot_id_fixed_format_valid(std::string_view(out, boot_id_length));
}

bool read_kernel_clock(clockid_t clock, clock_time& out) {
    ::timespec now{};
    if (::clock_gettime(clock, &now) != 0) return false;
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= nanoseconds_per_second) {
        return false;
    }
    out.seconds = static_cast<std::int64_t>(now.tv_sec);
    out.nanoseconds = static_cast<std::int32_t>(now.tv_nsec);
    return true;
}

}  // namespace

host_sample_result sample_host_clock() {
    host_sample_result result;  // defaults to unavailable: no fallback clock
    clock_sample sample{};
    if (!read_boot_id_fixed_format(sample.boot_id_before)) return result;
    if (!read_kernel_clock(CLOCK_BOOTTIME, sample.boot_before)) return result;
    if (!read_kernel_clock(CLOCK_REALTIME, sample.realtime)) return result;
    if (!read_kernel_clock(CLOCK_BOOTTIME, sample.boot_after)) return result;
    if (!read_boot_id_fixed_format(sample.boot_id_after)) return result;
    if (validate_clock_sample(sample) != sample_status::ok) return result;
    result.status = host_sample_status::ok;
    result.sample = sample;
    return result;
}

}  // namespace wls::controls::detail
