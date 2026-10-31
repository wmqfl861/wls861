#include "wls/native_foundation.hpp"

#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
int failure(std::string_view code, std::string message) {
    // Keep diagnostics bounded without cutting a multibyte Unicode scalar.
    if (message.size() > 1024) {
        std::size_t end = 1024;
        while ((static_cast<unsigned char>(message[end]) & 0xc0U) == 0x80U) --end;
        message.resize(end);
    }
    try {
        std::cerr << "{\"error\":" << wls::native::json_string(code) << ",\"message\":" <<
            wls::native::json_string(message) << "}\n";
    } catch (const std::exception&) {
        std::cerr << "{\"error\":\"invalid_diagnostic\",\"message\":\"diagnostic is not valid UTF-8\"}\n";
    }
    return 2;
}

template<class Convert>
int execute(int argc, Convert convert) {
    try {
        if (argc < 2 || argc > 129) throw wls::native::Error(wls::native::ErrorCode::InvalidInput,
            "a command with at most 128 arguments is required");
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) arguments.push_back(convert(index));
        std::cout << wls::native::run_command(arguments) << '\n';
        return 0;
    } catch (const wls::native::Error& error) {
        return failure(wls::native::error_name(error.code()), error.what());
    } catch (const wls::ConfigurationError& error) {
        return failure("configuration_rejected", error.what());
    } catch (const std::exception&) {
        return failure("runtime_error", "native foundation operation failed");
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    return execute(argc, [argv](int index) {
        const std::wstring_view value(argv[index]);
        return wls::native::utf16_to_utf8(std::u16string(value.begin(), value.end()));
    });
}
#else
int main(int argc, char** argv) {
    return execute(argc, [argv](int index) { return std::string(argv[index]); });
}
#endif
