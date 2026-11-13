#pragma once
#include "wls/runtime_types.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace wls::s1::operation_output_detail {
// Display policy only: NUL-containing or invalid UTF-8 bytes remain binary.
// JSON can encode NUL, but that does not make an arbitrary process stream text.
inline bool text_stream(std::string_view bytes) {
    if (bytes.find('\0') != std::string_view::npos) return false;
    try { (void)canonical_json(Json(bytes)); }
    catch (const Error&) { return false; }
    return true;
}

inline std::string base64(std::string_view bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(count));
    return encoded;
}

// All sizes and hashes describe the original retained bytes, never JSON escapes
// or base64. Preview allocation may leave a stream with no room for a codepoint.
inline Json page(std::string_view bytes, std::size_t offset, std::size_t limit,
        bool truncated, bool allow_empty = false) {
    if (bytes.size() > 1048576 || offset > bytes.size() || limit > 65536 || (!limit && !allow_empty))
        throw Error(ErrorCode::InvalidInput, "invalid retained output page");
    const bool utf8 = text_stream(bytes);
    const auto continuation = [&](std::size_t pos) {
        return pos < bytes.size() && (static_cast<unsigned char>(bytes[pos]) & 0xc0U) == 0x80U;
    };
    auto end = offset + std::min(limit, bytes.size() - offset);
    if (utf8) {
        if (continuation(offset)) throw Error(ErrorCode::InvalidInput, "output offset splits a UTF-8 codepoint");
        while (end > offset && continuation(end)) --end;
        if (end == offset && offset < bytes.size() && !allow_empty)
            throw Error(ErrorCode::InvalidInput, "output page cannot fit one UTF-8 codepoint");
    }
    const auto returned = bytes.substr(offset, end - offset);
    return boost::json::object{{"encoding", utf8 ? "utf-8" : "base64"},
        {"data", utf8 ? std::string(returned) : base64(returned)}, {"offset", offset},
        {"bytes_returned", returned.size()}, {"next_offset", end}, {"total_bytes", bytes.size()},
        {"stream_sha256", sha256_hex(bytes)}, {"page_sha256", sha256_hex(returned)},
        {"truncated", truncated}, {"eof", end == bytes.size()}};
}

inline Json preview(std::string_view stdout_bytes, std::string_view stderr_bytes, bool truncated) {
    auto out = page(stdout_bytes, 0, 8192, truncated, true);
    const auto used = out.at("bytes_returned").to_number<std::size_t>();
    auto err = page(stderr_bytes, 0, 8192 - used, truncated, true);
    const bool omitted = !out.at("eof").as_bool() || !err.at("eof").as_bool();
    return boost::json::object{{"stdout", std::move(out)}, {"stderr", std::move(err)},
        {"preview_truncated", omitted}};
}
} // namespace wls::s1::operation_output_detail
