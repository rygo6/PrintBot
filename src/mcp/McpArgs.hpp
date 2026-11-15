// print_bot MCP: helpers for reading tool arguments and building JSON schemas. Shared by the
// headless tools (src/mcp) and the live-GUI tools (src/slic3r/GUI/PrintBotMcpTools.cpp).
// GUI-free (no wxWidgets).
#ifndef printbot_mcp_McpArgs_hpp_
#define printbot_mcp_McpArgs_hpp_

#include <optional>
#include <string>
#include <vector>

#include "McpServer.hpp"

namespace printbot::mcp::args {

[[noreturn]] inline void bad_arg(const std::string &msg) { throw ToolError{"invalid_argument", msg}; }

inline std::string str_arg(const json &a, const char *key, bool required = false, const std::string &def = {})
{
    if (!a.contains(key) || a[key].is_null()) {
        if (required) bad_arg(std::string("missing required argument '") + key + "'");
        return def;
    }
    if (!a[key].is_string()) bad_arg(std::string("argument '") + key + "' must be a string");
    return a[key].get<std::string>();
}

inline std::optional<std::string> opt_str(const json &a, const char *key)
{
    if (!a.contains(key) || a[key].is_null()) return std::nullopt;
    if (!a[key].is_string()) bad_arg(std::string("argument '") + key + "' must be a string");
    return a[key].get<std::string>();
}

inline bool bool_arg(const json &a, const char *key, bool def)
{
    if (!a.contains(key) || a[key].is_null()) return def;
    if (!a[key].is_boolean()) bad_arg(std::string("argument '") + key + "' must be a boolean");
    return a[key].get<bool>();
}

inline std::optional<bool> opt_bool(const json &a, const char *key)
{
    if (!a.contains(key) || a[key].is_null()) return std::nullopt;
    return bool_arg(a, key, false);
}

inline double num_arg(const json &a, const char *key, double def)
{
    if (!a.contains(key) || a[key].is_null()) return def;
    if (!a[key].is_number()) bad_arg(std::string("argument '") + key + "' must be a number");
    return a[key].get<double>();
}

inline std::optional<double> opt_num(const json &a, const char *key)
{
    if (!a.contains(key) || a[key].is_null()) return std::nullopt;
    return num_arg(a, key, 0);
}

inline long long int_arg(const json &a, const char *key, bool required = false, long long def = 0)
{
    if (!a.contains(key) || a[key].is_null()) {
        if (required) bad_arg(std::string("missing required argument '") + key + "'");
        return def;
    }
    if (!a[key].is_number_integer() && !a[key].is_number_unsigned()) bad_arg(std::string("argument '") + key + "' must be an integer");
    return a[key].get<long long>();
}

inline std::vector<std::string> str_list_arg(const json &a, const char *key)
{
    std::vector<std::string> out;
    if (!a.contains(key) || a[key].is_null()) return out;
    if (a[key].is_string()) {
        out.push_back(a[key].get<std::string>());
        return out;
    }
    if (!a[key].is_array()) bad_arg(std::string("argument '") + key + "' must be an array of strings");
    for (const auto &x : a[key]) {
        if (!x.is_string()) bad_arg(std::string("argument '") + key + "' must be an array of strings");
        out.push_back(x.get<std::string>());
    }
    return out;
}

inline std::vector<long long> int_list_arg(const json &a, const char *key)
{
    std::vector<long long> out;
    if (!a.contains(key) || a[key].is_null()) return out;
    if (a[key].is_number_integer()) {
        out.push_back(a[key].get<long long>());
        return out;
    }
    if (!a[key].is_array()) bad_arg(std::string("argument '") + key + "' must be an array of integers");
    for (const auto &x : a[key]) {
        if (!x.is_number_integer()) bad_arg(std::string("argument '") + key + "' must be an array of integers");
        out.push_back(x.get<long long>());
    }
    return out;
}

inline json schema(json properties = json::object(), std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}

inline json prop(const char *type, const std::string &description) { return json{{"type", type}, {"description", description}}; }

inline json enum_prop(const std::vector<std::string> &values, const std::string &description)
{
    return json{{"type", "string"}, {"enum", values}, {"description", description}};
}

} // namespace printbot::mcp::args

#endif
