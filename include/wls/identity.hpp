#pragma once

#include <string>

namespace wls {
enum class Role { Root, Dispatcher, TeamLead, Worker };
// Supplied by a trusted host adapter, never decoded from model/tool arguments.
// The kernel also checks these fields against the persisted request/team owner.
struct Actor {
    Role role{Role::Root};
    std::string root_id;
    std::string request_id;
    std::string team_id;
    std::string principal_id;
};
} // namespace wls
