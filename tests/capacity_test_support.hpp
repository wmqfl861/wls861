#pragma once
#include "wls/store.hpp"
#include "test_support.hpp"
#include <sqlite3.h>

namespace test {
// Inject only inert business audit rows in a disposable database. Read the
// request's actual frozen watermark rather than assuming a historical 4096.
inline void fill_business_capacity(wls::s1::Store& store,
        const std::filesystem::path& path, const std::string& request_id) {
    const auto state = store.capacity_status(request_id);
    const auto remaining = state.at("limits").at("business_events").as_int64() -
        state.at("counts").at("business_events").as_int64();
    CHECK(remaining > 0);
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const auto sql = std::string("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<") +
        std::to_string(remaining) + ") INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) "
        "SELECT 'capacity-'||x,'" + request_id + "','fixture','runtime','capacity-'||x,printf('%064d',0),'{}','{}','fixture' FROM n";
    const auto rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    CHECK(rc == SQLITE_OK);
    const auto filled = store.capacity_status(request_id); // verifies cache invalidation
    CHECK(filled.at("counts").at("business_events") == filled.at("limits").at("business_events"));
}
}
