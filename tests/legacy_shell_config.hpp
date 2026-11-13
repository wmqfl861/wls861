#pragma once
#include "wls/config_loader.hpp"
#include "test_support.hpp"
#include <fstream>

namespace test {
// Historical tool suites explicitly select shell in a new disposable configuration.
inline wls::s1::ConfigCatalog legacy_shell_catalog(const std::filesystem::path& system,
        const std::filesystem::path& functions, const std::filesystem::path& prompts) {
    using namespace wls::s1;
    auto function = load_config_catalog(system, functions, prompts).freeze("general").json().at("function");
    if (function.at("team_templates").at("default") == function.at("team_templates").at("files"))
        function.at("team_templates").at("default") = function.at("team_templates").at("shell");
    TempDir fixture;
    std::filesystem::copy(functions, fixture.path / "functions", std::filesystem::copy_options::recursive);
    {
        std::ofstream output(fixture.path / "functions/general/function.yaml");
        output << canonical_json(function); CHECK(output.good());
    }
    return load_config_catalog(system, fixture.path / "functions", prompts);
}
} // namespace test
