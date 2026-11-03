#pragma once

#include "test_support.hpp"

#include <sqlite3.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace test::persistent_events {

// Register before opening the fixture's Store connection. RAISE(ABORT) is a
// constraint failure; this function instead exercises Store's real I/O-error
// classification without adding fault controls to a production interface.
inline void storage_fault(sqlite3_context* context, int, sqlite3_value**) noexcept {
    sqlite3_result_error(context, "injected persistent event storage fault", -1);
    sqlite3_result_error_code(context, SQLITE_IOERR);
}
inline int install_storage_fault(sqlite3* database, char**, const sqlite3_api_routines*) {
    return sqlite3_create_function_v2(database, "n5_test_storage_fault", 0,
        SQLITE_UTF8 | SQLITE_INNOCUOUS, nullptr, storage_fault, nullptr, nullptr, nullptr);
}
class StorageFault final {
public:
    StorageFault() {
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_storage_fault)) == SQLITE_OK);
    }
    ~StorageFault() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(install_storage_fault));
    }
    StorageFault(const StorageFault&) = delete;
    StorageFault& operator=(const StorageFault&) = delete;
};

// SQLite invokes a WAL hook after a transaction has committed. Returning an
// error makes the API report failure even though the commit is durable. This
// models commit-outcome uncertainty without exposing fault controls through
// Store, changing its journal settings, or guessing a successful write.
class CommitOutcomeFault;
inline CommitOutcomeFault* active_commit_outcome_fault{};
inline int committed_wal_fault(void*, sqlite3*, const char*, int) noexcept;
inline void arm_committed_wal_fault(sqlite3_context*, int, sqlite3_value**) noexcept;
inline int install_committed_wal_fault(sqlite3* database, char**, const sqlite3_api_routines*) {
    // sqlite3_open's automatic checkpoint setup may replace an extension's
    // WAL hook. A fixture trigger installs it on the actual writer connection
    // inside the targeted transaction, after connection initialization.
    return sqlite3_create_function_v2(database, "n5_test_arm_commit_outcome", 0,
        SQLITE_UTF8 | SQLITE_INNOCUOUS, nullptr, arm_committed_wal_fault, nullptr, nullptr, nullptr);
}
class CommitOutcomeFault final {
public:
    CommitOutcomeFault() {
        CHECK(active_commit_outcome_fault == nullptr);
        active_commit_outcome_fault = this;
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_committed_wal_fault)) == SQLITE_OK);
    }
    ~CommitOutcomeFault() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(install_committed_wal_fault));
        active_commit_outcome_fault = nullptr;
    }
    void arm(const std::filesystem::path& path) {
        CHECK(!armed_ && hits_ == 0);
        database_path_ = std::filesystem::canonical(path).string(); armed_ = true;
    }
    void install(sqlite3* database) noexcept {
        const auto* filename = sqlite3_db_filename(database, "main");
        if (armed_ && filename && std::strcmp(filename, database_path_.c_str()) == 0) {
            sqlite3_wal_hook(database, committed_wal_fault, nullptr); ++installed_;
        }
    }
    int observe(sqlite3* database, const char* schema, int pages) noexcept {
        const auto* filename = sqlite3_db_filename(database, schema);
        if (!armed_ || !filename || std::strcmp(filename, database_path_.c_str()) != 0) return SQLITE_OK;
        armed_ = false; ++hits_; committed_pages_ = pages;
        return SQLITE_IOERR_WRITE;
    }
    int hits() const noexcept { return hits_; }
    int installed() const noexcept { return installed_; }
    int committed_pages() const noexcept { return committed_pages_; }
    CommitOutcomeFault(const CommitOutcomeFault&) = delete;
    CommitOutcomeFault& operator=(const CommitOutcomeFault&) = delete;
private:
    std::string database_path_;
    bool armed_{};
    int hits_{}, installed_{}, committed_pages_{};
};
inline void arm_committed_wal_fault(sqlite3_context* context, int, sqlite3_value**) noexcept {
    if (active_commit_outcome_fault) active_commit_outcome_fault->install(sqlite3_context_db_handle(context));
    sqlite3_result_null(context);
}
inline int committed_wal_fault(void*, sqlite3* database, const char* schema, int pages) noexcept {
    return active_commit_outcome_fault ? active_commit_outcome_fault->observe(database, schema, pages) : SQLITE_OK;
}

} // namespace test::persistent_events
