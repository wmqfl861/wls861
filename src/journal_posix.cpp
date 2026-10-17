#include "wls/journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wls {
namespace {

constexpr std::size_t header_size = 24;
constexpr std::array<unsigned char, 12> fixed_prefix{
    'W', 'L', 'S', 'J', 'N', 'L', '\r', '\n', 1, 0, 0, 0};
using Header = std::array<unsigned char, header_size>;

constexpr auto make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        auto value = index;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) ? 0x82f63b78U : 0U);
        table[index] = value;
    }
    return table;
}

constexpr auto crc_table = make_crc_table();

std::uint32_t crc32c(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index)
        crc = crc_table[(crc ^ bytes[index]) & 0xffU] ^ (crc >> 8U);
    return crc ^ 0xffffffffU;
}

void put_u32(Header& header, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        header[offset + index] = static_cast<unsigned char>(value >> (index * 8U));
}

std::uint32_t get_u32(const Header& header, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(header[offset + index]) << (index * 8U);
    return value;
}

Header make_header(std::string_view transaction) {
    Header header{};
    std::copy(fixed_prefix.begin(), fixed_prefix.end(), header.begin());
    put_u32(header, 12, static_cast<std::uint32_t>(transaction.size()));
    put_u32(header, 16, crc32c(transaction.data(), transaction.size()));
    put_u32(header, 20, crc32c(header.data(), 20));
    return header;
}

[[noreturn]] void fail_system(const char* operation, int error = errno) {
    throw JournalError(std::string("journal ") + operation + ": " +
                       std::error_code(error, std::generic_category()).message());
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
    void set(int fd) noexcept { fd_ = fd; }
private:
    int fd_;
};

void sync_data(int fd) {
    int result;
    do { result = ::fdatasync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) fail_system("fdatasync");
}

void sync_directory(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) fail_system("parent directory fsync");
}

void read_exact(int fd, void* buffer, std::size_t size, off_t offset) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (size != 0) {
        const auto amount = ::pread(fd, bytes, size, offset);
        if (amount < 0) {
            if (errno == EINTR) continue;
            fail_system("pread");
        }
        if (amount == 0) throw JournalError("journal changed while recovering");
        const auto read_size = static_cast<std::size_t>(amount);
        bytes += read_size;
        size -= read_size;
        offset += amount;
    }
}

void write_exact(int fd, const void* buffer, std::size_t size, off_t offset) {
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    while (size != 0) {
        const auto amount = ::pwrite(fd, bytes, size, offset);
        if (amount < 0) {
            if (errno == EINTR) continue;
            fail_system("pwrite");
        }
        if (amount == 0) throw JournalError("journal pwrite made no progress");
        const auto write_size = static_cast<std::size_t>(amount);
        bytes += write_size;
        size -= write_size;
        offset += amount;
    }
}

// Validate every available fixed byte, even in a partial tail header. A fully
// available length must be bounded before it is used to allocate or truncate.
void validate_header_prefix(const Header& header, std::size_t available) {
    const auto prefix_size = std::min(available, fixed_prefix.size());
    for (std::size_t index = 0; index < prefix_size; ++index) {
        if (header[index] != fixed_prefix[index])
            throw JournalError("journal has invalid magic, version, or flags");
    }
    if (available >= 16 && get_u32(header, 12) > DurableJournal::max_transaction_bytes)
        throw JournalError("journal transaction length exceeds the 16 MiB limit");
}

} // namespace

class DurableJournal::Impl final {
public:
    explicit Impl(const std::filesystem::path& path) : owner_pid_(::getpid()) {
        if (path.empty() || path.native().find('\0') != std::string::npos)
            throw JournalError("journal path must be nonempty and contain no NUL byte");
        const auto filename = path.filename().native();
        if (filename.empty() || filename == "." || filename == "..")
            throw JournalError("journal path must name a file");
        const auto parent = path.has_parent_path() ? path.parent_path() :
                                                   std::filesystem::path(".");
        // Pin the parent directory so file creation and directory fsync refer
        // to the same directory, even if its path is renamed concurrently.
        FileDescriptor directory(::open(parent.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (directory.get() < 0) fail_system("open parent directory");

        int raw_fd = ::openat(directory.get(), filename.c_str(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
            S_IRUSR | S_IWUSR);
        if (raw_fd < 0 && errno == EEXIST)
            raw_fd = ::openat(directory.get(), filename.c_str(),
                O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (raw_fd < 0) fail_system("open file");
        fd_.set(raw_fd);

        struct stat metadata{};
        if (::fstat(fd_.get(), &metadata) < 0) fail_system("fstat");
        if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0)
            throw JournalError("journal must be a regular file");

        int lock_result;
        do { lock_result = ::flock(fd_.get(), LOCK_EX | LOCK_NB); }
        while (lock_result < 0 && errno == EINTR);
        if (lock_result < 0) fail_system("exclusive lock");

        // The size check belongs after locking: another owner might have
        // finished an append between our open/fstat and successful flock.
        if (::fstat(fd_.get(), &metadata) < 0) fail_system("fstat after lock");
        recover(metadata.st_size);
        // Sync even on reopen: complete frames from an ambiguous prior append
        // become durable, and a creator/open-existing race cannot skip the
        // directory sync that makes the newly created name durable.
        sync_data(fd_.get());
        sync_directory(directory.get());
    }

    std::vector<std::string> replay() const {
        check_process();
        std::lock_guard lock(mutex_);
        check_healthy();
        return transactions_;
    }

    void append(std::string_view transaction) {
        check_process();
        std::lock_guard lock(mutex_);
        check_healthy();
        try {
            if (transaction.size() > DurableJournal::max_transaction_bytes)
                throw JournalError("journal transaction exceeds the 16 MiB limit");
            const auto frame_size = header_size + transaction.size();
            if (end_ > std::numeric_limits<off_t>::max() - static_cast<off_t>(frame_size))
                throw JournalError("journal file offset limit exceeded");

            // Finish every potentially allocating cache operation before the
            // first write. Moving the reserved string after sync cannot allocate.
            std::string owned(transaction);
            if (transactions_.size() == transactions_.capacity()) {
                const auto current = transactions_.capacity();
                const auto maximum = transactions_.max_size();
                if (current == maximum)
                    throw JournalError("journal transaction count limit exceeded");
                const auto next = current == 0 ? std::size_t{1} :
                    (current > maximum / 2 ? maximum : current * 2);
                transactions_.reserve(next);
            }
            const auto header = make_header(owned);
            write_exact(fd_.get(), header.data(), header.size(), end_);
            write_exact(fd_.get(), owned.data(), owned.size(),
                        end_ + static_cast<off_t>(header.size()));
            sync_data(fd_.get());
            transactions_.push_back(std::move(owned));
            end_ += static_cast<off_t>(frame_size);
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }

private:
    void check_process() const {
        // flock locks are inherited across fork. A copied object must never
        // be used as an independent writer in the child process.
        if (::getpid() != owner_pid_)
            throw JournalError("journal object cannot be used after fork; reopen it");
    }

    void check_healthy() const {
        if (poisoned_)
            throw JournalError("journal is poisoned by a failed append; close and recover");
    }

    void recover(off_t file_size) {
        while (end_ < file_size) {
            Header header{};
            const auto remaining = file_size - end_;
            const auto available = static_cast<std::size_t>(
                std::min(remaining, static_cast<off_t>(header_size)));
            read_exact(fd_.get(), header.data(), available, end_);
            validate_header_prefix(header, available);
            if (available < header_size) { truncate_tail(); return; }
            if (get_u32(header, 20) != crc32c(header.data(), 20))
                throw JournalError("journal header checksum mismatch");

            const auto length = static_cast<std::size_t>(get_u32(header, 12));
            if (static_cast<off_t>(length) > remaining - static_cast<off_t>(header_size)) {
                truncate_tail();
                return;
            }
            std::string transaction(length, '\0');
            read_exact(fd_.get(), transaction.data(), length,
                       end_ + static_cast<off_t>(header_size));
            if (get_u32(header, 16) != crc32c(transaction.data(), transaction.size()))
                throw JournalError("journal payload checksum mismatch");
            transactions_.push_back(std::move(transaction));
            end_ += static_cast<off_t>(header_size + length);
        }
    }

    void truncate_tail() {
        int result;
        do { result = ::ftruncate(fd_.get(), end_); } while (result < 0 && errno == EINTR);
        if (result < 0) fail_system("truncate incomplete tail");
        // The constructor syncs the repaired file before exposing the object.
    }

    FileDescriptor fd_;
    const pid_t owner_pid_;
    mutable std::mutex mutex_;
    std::vector<std::string> transactions_;
    off_t end_ = 0;
    bool poisoned_ = false;
};

DurableJournal::DurableJournal(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}

DurableJournal::~DurableJournal() = default;

std::vector<std::string> DurableJournal::replay() const { return impl_->replay(); }

void DurableJournal::append(std::string_view transaction) { impl_->append(transaction); }

} // namespace wls
