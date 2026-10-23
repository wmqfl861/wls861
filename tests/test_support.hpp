#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace test {
inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) throw std::runtime_error(std::string(file) + ":" +
        std::to_string(line) + ": " + expression);
}
template<class Error = std::exception, class Function>
void throws(Function&& function) {
    bool caught = false;
    try { function(); } catch (const Error&) { caught = true; }
    if (!caught) throw std::runtime_error("expected exception was not thrown");
}
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "wls861-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~TempDir() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::filesystem::path path;
};
using Case = std::pair<std::string, std::function<void()>>;
inline int run(const std::vector<Case>& cases) {
    // Tests own only disposable fixtures. Do not rely on the invoking shell's
    // umask for their modes; production permission guards stay unchanged.
    const auto original_umask = ::umask(0077);
    int failed = 0;
    for (const auto& [name, function] : cases) {
        try { function(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "RESULT " << cases.size() - static_cast<std::size_t>(failed)
              << " passed, " << failed << " failed\n";
    ::umask(original_umask);
    return failed ? 1 : 0;
}
} // namespace test

#define CHECK(expression) ::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
