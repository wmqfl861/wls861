#pragma once

#include "test_support.hpp"

#include <sqlite3.h>

namespace wls::test::persistent_events {

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

} // namespace wls::test::persistent_events
