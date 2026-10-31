#pragma once
#include "wls/native_foundation.hpp"
#include <functional>
#include <iostream>
#include <utility>

namespace native_test {
inline void check(bool value, const char* expression, int line) {
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
template<class Exception, class Function>
void rejects(Function&& function) {
    bool rejected = false;
    try { function(); } catch (const Exception&) { rejected = true; }
    check(rejected, "expected rejection", __LINE__);
}
using Case = std::pair<const char*, std::function<void()>>;
inline int run(const std::vector<Case>& cases) {
    std::size_t failures = 0;
    for (const auto& [name, function] : cases) {
        try { function(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "RESULT " << cases.size() - failures << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
} // namespace native_test
#define CHECK(value) ::native_test::check(static_cast<bool>(value), #value, __LINE__)
