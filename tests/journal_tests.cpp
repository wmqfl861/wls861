#include "wls/journal.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {

using wls::DurableJournal;
using wls::JournalError;
constexpr std::size_t header_size = 24;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.is_open());
    std::string contents{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
    CHECK(!input.bad());
    return contents;
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    CHECK(output);
}

class ChildProcess final {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) { CHECK(pid_ > 0); }
    ~ChildProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    int wait() {
        int status = 0;
        pid_t result;
        do { result = ::waitpid(pid_, &status, 0); }
        while (result < 0 && errno == EINTR);
        CHECK(result == pid_);
        pid_ = -1;
        return status;
    }
private:
    pid_t pid_;
};

void check_successful_child(ChildProcess& child) {
    const auto status = child.wait();
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

void format_and_round_trip() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    // Independent fixed wire fixture. CRC32C("123456789") = 0xe3069283;
    // CRC32C of these first 20 header bytes = 0x3aac55f4.
    constexpr std::array<unsigned char, header_size> known_header{
        0x57, 0x4c, 0x53, 0x4a, 0x4e, 0x4c, 0x0d, 0x0a,
        0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
        0x83, 0x92, 0x06, 0xe3, 0xf4, 0x55, 0xac, 0x3a};
    std::string fixture(reinterpret_cast<const char*>(known_header.data()),
                        known_header.size());
    fixture += "123456789";
    {
        DurableJournal journal(path);
        CHECK(journal.replay().empty());
        journal.append("123456789");
        CHECK(read_file(path) == fixture);
        journal.append("");
        journal.append(std::string("binary\0\xff\n", 9));
        auto snapshot = journal.replay();
        snapshot[0] = "changed copy";
        CHECK(journal.replay()[0] == "123456789");
    }
    {
        DurableJournal journal(path);
        const auto records = journal.replay();
        CHECK(records.size() == 3);
        CHECK(records[0] == "123456789");
        CHECK(records[1].empty());
        CHECK(records[2] == std::string("binary\0\xff\n", 9));
    }
    const auto imported = directory.path / "independent-fixture";
    write_file(imported, fixture);
    DurableJournal journal(imported);
    CHECK(journal.replay() == std::vector<std::string>{"123456789"});
}

void every_torn_tail_position() {
    test::TempDir directory;
    const auto original = directory.path / "original";
    const std::string prefix = "committed";
    const std::string tail("ta\0il!", 6);
    {
        DurableJournal journal(original);
        journal.append(prefix);
        journal.append(tail);
    }
    const auto bytes = read_file(original);
    const auto prefix_size = header_size + prefix.size();
    const auto tail_size = header_size + tail.size();
    CHECK(bytes.size() == prefix_size + tail_size);
    for (std::size_t retained = 0; retained < tail_size; ++retained) {
        const auto path = directory.path / ("torn-" + std::to_string(retained));
        write_file(path, std::string_view(bytes).substr(0, prefix_size + retained));
        {
            DurableJournal recovered(path);
            CHECK(recovered.replay() == std::vector<std::string>{prefix});
            CHECK(std::filesystem::file_size(path) == prefix_size);
            recovered.append("after recovery");
        }
        DurableJournal reopened(path);
        const std::vector<std::string> expected{prefix, "after recovery"};
        CHECK(reopened.replay() == expected);
    }
}

void complete_corruption_fails_without_truncation() {
    test::TempDir directory;
    const auto original = directory.path / "original";
    const std::string prefix = "keep me";
    {
        DurableJournal journal(original);
        journal.append(prefix);
        journal.append("payload");
    }
    const auto original_bytes = read_file(original);
    const auto second_frame = header_size + prefix.size();
    // Invalid magic, unsupported version, reserved flag, changed length,
    // changed header checksum, and damaged complete payload are all fatal.
    const std::array<std::size_t, 6> positions{0, 8, 10, 12, 20, header_size + 1};
    for (const auto position : positions) {
        auto damaged = original_bytes;
        damaged[second_frame + position] ^= 0x40;
        const auto path = directory.path / ("corrupt-" + std::to_string(position));
        write_file(path, damaged);
        test::throws<JournalError>([&] { DurableJournal journal(path); });
        CHECK(read_file(path) == damaged);
    }

    const auto garbage_path = directory.path / "invalid-partial-magic";
    const auto garbage = original_bytes.substr(0, second_frame) + "XX";
    write_file(garbage_path, garbage);
    test::throws<JournalError>([&] { DurableJournal journal(garbage_path); });
    CHECK(read_file(garbage_path) == garbage);
}

void abnormal_length_is_rejected() {
    test::TempDir directory;
    // Both headers have correct header checksums but lengths above the limit.
    constexpr std::array<std::array<unsigned char, header_size>, 2> headers{{
        {0x57, 0x4c, 0x53, 0x4a, 0x4e, 0x4c, 0x0d, 0x0a,
         0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
         0x00, 0x00, 0x00, 0x00, 0x9c, 0x2c, 0x41, 0x5d},
        {0x57, 0x4c, 0x53, 0x4a, 0x4e, 0x4c, 0x0d, 0x0a,
         0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
         0x00, 0x00, 0x00, 0x00, 0x62, 0x73, 0xbb, 0x5f}
    }};
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto path = directory.path / ("oversized-" + std::to_string(index));
        const std::string bytes(reinterpret_cast<const char*>(headers[index].data()),
                                headers[index].size());
        write_file(path, bytes);
        test::throws<JournalError>([&] { DurableJournal journal(path); });
        CHECK(read_file(path) == bytes);
    }
}

void exclusive_writer_and_fork_ownership() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    auto owner = std::make_unique<DurableJournal>(path);
    owner->append("before fork");
    test::throws<JournalError>([&] { DurableJournal second(path); });
    const auto pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        try {
            test::throws<JournalError>([&] { owner->append("inherited writer"); });
            test::throws<JournalError>([&] { static_cast<void>(owner->replay()); });
            owner.reset();
            // Closing the child's inherited descriptor must not unlock the
            // open file description that the parent still owns.
            test::throws<JournalError>([&] { DurableJournal second(path); });
            ::_exit(0);
        } catch (...) { ::_exit(2); }
    }
    ChildProcess child(pid);
    check_successful_child(child);
    owner->append("still owned by parent");
    owner.reset();
    DurableJournal replacement(path);
    const std::vector<std::string> expected{"before fork", "still owned by parent"};
    CHECK(replacement.replay() == expected);
}

void sigkill_preserves_acknowledged_append() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    {
        DurableJournal journal(path);
        journal.append("before child");
    }
    int pipe_fds[2]{};
    CHECK(::pipe2(pipe_fds, O_CLOEXEC) == 0);
    const auto pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ::close(pipe_fds[0]);
        try {
            DurableJournal journal(path);
            journal.append("acknowledged before SIGKILL");
            const char ready = 'R';
            if (::write(pipe_fds[1], &ready, 1) != 1) ::_exit(3);
            for (;;) ::pause();
        } catch (...) { ::_exit(2); }
    }
    ChildProcess child(pid);
    ::close(pipe_fds[1]);
    struct pollfd poll_fd{pipe_fds[0], POLLIN, 0};
    int poll_result;
    do { poll_result = ::poll(&poll_fd, 1, 5000); }
    while (poll_result < 0 && errno == EINTR);
    char ready = 0;
    const auto amount = poll_result > 0 ? ::read(pipe_fds[0], &ready, 1) : -1;
    ::close(pipe_fds[0]);
    CHECK(poll_result > 0);
    CHECK(amount == 1);
    CHECK(ready == 'R');
    CHECK(::kill(pid, SIGKILL) == 0);
    const auto status = child.wait();
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    DurableJournal recovered(path);
    const std::vector<std::string> expected{"before child", "acknowledged before SIGKILL"};
    CHECK(recovered.replay() == expected);
    recovered.append("after SIGKILL");
}

void partial_write_poison_and_recovery() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    const auto pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        try {
            DurableJournal journal(path);
            journal.append("committed");
            struct rlimit limit{};
            CHECK(::getrlimit(RLIMIT_FSIZE, &limit) == 0);
            limit.rlim_cur = static_cast<rlim_t>(std::filesystem::file_size(path) +
                                                header_size + 3);
            CHECK(::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
            CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
            // The header and three payload bytes fit; the remaining pwrite
            // fails with EFBIG. This exercises an actual OS partial write.
            test::throws<JournalError>([&] { journal.append(std::string(100, 'x')); });
            test::throws<JournalError>([&] { journal.append(""); });
            test::throws<JournalError>([&] { static_cast<void>(journal.replay()); });
            ::_exit(0);
        } catch (...) { ::_exit(2); }
    }
    ChildProcess child(pid);
    check_successful_child(child);
    const auto committed_size = header_size + std::string_view("committed").size();
    CHECK(std::filesystem::file_size(path) == committed_size + header_size + 3);
    DurableJournal recovered(path);
    CHECK(recovered.replay() == std::vector<std::string>{"committed"});
    CHECK(std::filesystem::file_size(path) == committed_size);
    recovered.append("after partial write");
}

void maximum_payload_and_oversize_poison() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    const std::string maximum(DurableJournal::max_transaction_bytes, '\xa5');
    {
        DurableJournal journal(path);
        journal.append(maximum);
        CHECK(journal.replay() == std::vector<std::string>{maximum});
        const auto original_size = std::filesystem::file_size(path);
        const std::string too_large(DurableJournal::max_transaction_bytes + 1, 'x');
        test::throws<JournalError>([&] { journal.append(too_large); });
        test::throws<JournalError>([&] { journal.append("must remain rejected"); });
        CHECK(std::filesystem::file_size(path) == original_size);
    }
    DurableJournal recovered(path);
    CHECK(recovered.replay() == std::vector<std::string>{maximum});
    recovered.append("");
    CHECK(recovered.replay().size() == 2);
}

void safe_file_opening() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    {
        DurableJournal journal(path);
        struct stat metadata{};
        CHECK(::stat(path.c_str(), &metadata) == 0);
        CHECK((metadata.st_mode & 0077) == 0);
        CHECK(S_ISREG(metadata.st_mode));
    }
    const auto symlink = directory.path / "symlink";
    std::filesystem::create_symlink(path, symlink);
    test::throws<JournalError>([&] { DurableJournal journal(symlink); });
    test::throws<JournalError>([&] { DurableJournal journal(directory.path); });
    test::throws<JournalError>([&] { DurableJournal journal(directory.path / "missing" / "file"); });
    test::throws<JournalError>([&] { DurableJournal journal(std::filesystem::path{}); });
    const auto fifo = directory.path / "fifo";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    test::throws<JournalError>([&] { DurableJournal journal(fifo); });
    const auto parent_link = directory.path / "directory-link";
    std::filesystem::create_directory_symlink(directory.path, parent_link);
    test::throws<JournalError>([&] { DurableJournal journal(parent_link / "journal"); });
}

void concurrent_append_and_replay() {
    test::TempDir directory;
    const auto path = directory.path / "journal";
    constexpr std::size_t workers = 6;
    constexpr std::size_t per_worker = 32;
    std::vector<std::string> expected_replay;
    {
        DurableJournal journal(path);
        std::atomic<bool> start{false};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                try {
                    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    std::size_t last_snapshot_size = 0;
                    for (std::size_t index = 0; index < per_worker; ++index) {
                        journal.append(std::to_string(worker) + ":" + std::to_string(index));
                        if (index % 4 == 0) {
                            const auto snapshot = journal.replay();
                            CHECK(snapshot.size() >= last_snapshot_size);
                            last_snapshot_size = snapshot.size();
                        }
                    }
                } catch (...) {
                    std::lock_guard lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        if (failure) std::rethrow_exception(failure);
        expected_replay = journal.replay();
        CHECK(expected_replay.size() == workers * per_worker);
        const std::set<std::string> unique(expected_replay.begin(), expected_replay.end());
        CHECK(unique.size() == workers * per_worker);
        std::array<std::size_t, workers> next{};
        for (const auto& record : expected_replay) {
            const auto separator = record.find(':');
            CHECK(separator != std::string::npos);
            const auto worker = std::stoul(record.substr(0, separator));
            const auto index = std::stoul(record.substr(separator + 1));
            CHECK(worker < workers);
            CHECK(index == next[worker]);
            ++next[worker];
        }
        for (const auto count : next) CHECK(count == per_worker);
    }
    DurableJournal recovered(path);
    CHECK(recovered.replay() == expected_replay);
}

} // namespace

int main() {
    return test::run({
        {"version 1 format, empty and binary round trip", format_and_round_trip},
        {"recovery at every incomplete tail byte", every_torn_tail_position},
        {"complete corruption fails without truncation", complete_corruption_fails_without_truncation},
        {"checksummed abnormal lengths fail closed", abnormal_length_is_rejected},
        {"exclusive writer and inherited fork ownership", exclusive_writer_and_fork_ownership},
        {"SIGKILL recovery preserves acknowledged append", sigkill_preserves_acknowledged_append},
        {"real partial write poisons writer and recovers", partial_write_poison_and_recovery},
        {"maximum payload boundary and oversize poisoning", maximum_payload_and_oversize_poison},
        {"file mode, symlink, directory and FIFO guards", safe_file_opening},
        {"concurrent append and replay preserve order", concurrent_append_and_replay}
    });
}
