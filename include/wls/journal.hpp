#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wls {

class JournalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Linux, single-host journal. A live object owns the journal's exclusive lock.
// append() and replay() are thread-safe; destruction requires callers to join.
// A failed append poisons this instance. Close and recover before continuing;
// an I/O error does not prove that the attempted transaction is absent on disk.
class DurableJournal final {
public:
    static constexpr std::size_t max_transaction_bytes = 16U * 1024U * 1024U;

    explicit DurableJournal(const std::filesystem::path& path);
    ~DurableJournal();

    DurableJournal(const DurableJournal&) = delete;
    DurableJournal& operator=(const DurableJournal&) = delete;
    DurableJournal(DurableJournal&&) = delete;
    DurableJournal& operator=(DurableJournal&&) = delete;

    [[nodiscard]] std::vector<std::string> replay() const;
    void append(std::string_view transaction);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls
