#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace printbot::mcp {

enum class AuthorizationResult { consumed, reused, invalid, unavailable };

inline AuthorizationResult consume_print_authorization(const std::filesystem::path &directory, const std::string &id)
{
    if (id.empty() || id.size() > 128) return AuthorizationResult::invalid;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return AuthorizationResult::invalid;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return AuthorizationResult::unavailable;
    const bool created = std::filesystem::create_directory(directory / ("request-" + id), error);
    if (error) return AuthorizationResult::unavailable;
    return created ? AuthorizationResult::consumed : AuthorizationResult::reused;
}

}
