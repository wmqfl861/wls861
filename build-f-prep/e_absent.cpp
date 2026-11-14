#include "protocol_test_support.hpp"
#include <iostream>
int main() {
    using namespace wls::s1;
    auto input = wls::test::protocol::context();
    input.tools.front().description = builtin_tool_description("note");
    input.tools.front().definition.at("properties").at("message").as_object()["description"] = "Observation to retain in this synthetic fixture.";
    auto r = wls::test::protocol::request();
    r.context = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    std::cout << sha256_hex(canonical_json(to_json(r.context))) << '\n' << r.context.prefix_sha256 << '\n' << r.context.logical_input_sha256 << '\n' << r.context.cache_identity << '\n' << sha256_hex(canonical_json(to_json(r))) << '\n' << encode_responses_request(wls::test::protocol::profile(), r).body_sha256 << '\n';
}
