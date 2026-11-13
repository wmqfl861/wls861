#pragma once
#include "wls/runtime_types.hpp"

namespace test {
// Derive a new v3 response from a legacy fixture; never rewrite the fixture.
inline wls::s1::Json v3_fake_script(wls::s1::Json value) {
    if (value.is_array()) {
        for (auto& child : value.as_array()) child = v3_fake_script(child);
    } else if (value.is_object()) {
        const auto* name = value.as_object().if_contains("name");
        if (name && (*name == "submit_plan" || *name == "submit_amendment_plan")) {
            auto& args = value.at("arguments");
            auto& plan = *name == "submit_plan" ? args : args.at("plan");
            for (auto& node : plan.at("nodes").as_array())
                node.at("brief").at("title") = node.at("title");
        }
        for (auto& child : value.as_object()) child.value() = v3_fake_script(child.value());
    }
    return value;
}
} // namespace test
