// Error/HMS decode tables.
//
// err_code values (namespace obn::err xerr rows): paraphrased from the
// 111-entry `xerr` table in ATILLLLITA/OpenBambuFarmAPI `farm-errors.md`
// (https://github.com/ATILLLLITA/OpenBambuFarmAPI, licensed under the
// GNU Free Documentation License 1.3 with no Invariant Sections and no
// Front-Cover / Back-Cover Texts). Messages in the xerr table below are
// OUR paraphrases of protocol facts -- numeric codes, HTTP statuses and
// the code-reuse structure are verbatim protocol facts; no GFDL text is
// copied here. Adopted 2026-10-01.
//
// HMS / print_error messages: adopted under the MIT License,
// Copyright (c) 2026 Sean Callan, from doomspork/PandaSpy
// (https://github.com/doomspork/PandaSpy, file
// crates/pandaspy-proto/assets/hms/en.json), 552030 bytes,
// SHA-256 B7FD5358F3163B9B424FF4CD8D2A1F1B15519EFC5CFCA3D8ED31378CE05F3B64.
// The generated data in src/err_table_data.cpp carries the full MIT
// notice (permission text verbatim) at the data adoption site.

#include "obn/err_table.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "obn/json_lite.hpp"

namespace obn::err {

namespace {

// Our paraphrases of the xerr rows (facts only: the numeric code and the
// HTTP status - carried as a trailing comment - are verbatim protocol
// facts; every message below is our own one-sentence wording). Sorted by
// numeric code; rows sharing a reused code (1018, 1022, 1043, 1063) sit
// adjacent in source-table row order so describe_err_code joins them in
// that order with "; ". Full census: 113 = 111 REST rows (the 111-entry
// source table, ERR-01 criterion 1) + 2 MQTT-security rows.
struct XerrRow {
    unsigned long code;
    const char* message;
};

const XerrRow kXerrRows[] = {
    {1, "The server hit an unexpected internal fault while handling the request."}, // HTTP 500
    {2, "The server's database layer failed while processing the request."}, // HTTP 500
    {3, "This client has no activation recorded on this server."}, // HTTP 403
    {4, "The request was malformed and could not be understood."}, // HTTP 400
    {5, "The request itself is invalid as sent."}, // HTTP 400
    {6, "The supplied authentication token is not recognized."}, // HTTP 401
    {7, "The authentication token has passed its validity window."}, // HTTP 401
    {8, "The client must be updated before it can talk to this server."}, // HTTP 426
    {9, "The server must be updated before this client version can talk to it."}, // HTTP 426
    {10, "Client and server protocol versions are incompatible."}, // HTTP 400
    {11, "A supplied parameter falls outside the permitted range."}, // HTTP 400
    {12, "The referenced 3MF file is not present in the expected folder."}, // HTTP 400
    {13, "Collecting the support log took too long and timed out."}, // HTTP 408
    {14, "A filesystem operation failed on the server."}, // HTTP 500
    {15, "The device is not ready to start a firmware upgrade."}, // HTTP 400
    {16, "A firmware upgrade for this device is already scheduled."}, // HTTP 400
    {17, "The firmware download failed before completion."}, // HTTP 400
    {18, "Waiting for the firmware download to finish timed out."}, // HTTP 408
    {19, "The firmware upgrade did not complete within the allowed time."}, // HTTP 408
    {20, "The firmware upgrade did not succeed."}, // HTTP 500
    {21, "The firmware upgrade was requested from an unexpected state."}, // HTTP 400
    {22, "A security check rejected the operation."}, // HTTP 400
    {23, "A supplied parameter could not be accepted as given."}, // HTTP 400
    {1000, "The folder still contains items, so it cannot be removed."}, // HTTP 400
    {1001, "A folder with that name already exists."}, // HTTP 400
    {1002, "The referenced folder does not exist."}, // HTTP 400
    {1003, "The uploaded 3MF is not a G-code project file."}, // HTTP 400
    {1004, "The uploaded G-code 3MF does not contain exactly one plate."}, // HTTP 400
    {1005, "The bridged MQTT command got no reply from the printer within the request window."}, // HTTP 408
    {1006, "That device is no longer bound to this account."}, // HTTP 400
    {1007, "No tag matching the query was found."}, // HTTP 400
    {1008, "The firmware file for this request is missing."}, // HTTP 400
    {1009, "A task is already running for this job."}, // HTTP 400
    {1010, "The requested firmware does not exist."}, // HTTP 400
    {1012, "The firmware file failed validation."}, // HTTP 400
    {1013, "A firmware with that identity already exists."}, // HTTP 409
    {1014, "The uploaded 3MF archive is corrupt or unreadable."}, // HTTP 400
    {1015, "The supplied bulk option is not supported."}, // HTTP 400
    {1016, "Another bulk operation is still running."}, // HTTP 400
    {1017, "No bulk action with that identifier was found."}, // HTTP 404
    {1018, "The presented licence is not valid for this server."}, // HTTP 400
    {1018, "The activation uid was rejected as invalid."}, // HTTP 409
    {1019, "The launch request targets a task that is outdated relative to the ongoing one."}, // HTTP 409
    {1020, "The launch failed because the task has already finished or was terminated."}, // HTTP 409
    {1021, "The launch failed because no unused subtask was available."}, // HTTP 400
    {1022, "Launch rejected because the target device is suspended."}, // HTTP 409
    {1022, "Launch rejected because the task has already been started."}, // HTTP 400
    {1023, "The supplied username and password do not match an account."}, // HTTP 400
    {1024, "An account with that username already exists."}, // HTTP 400
    {1025, "This account is not permitted to perform the action."}, // HTTP 403
    {1026, "Login attempts are arriving too fast; slow down and retry."}, // HTTP 429
    {1027, "Cloud-account login is refused because this server does not support it."}, // HTTP 403
    {1028, "The Bambu account login attempt failed."}, // HTTP 400
    {1029, "Binding the printer to the account failed."}, // HTTP 400
    {1030, "The update was skipped because the task is not in progress."}, // HTTP 400
    {1031, "The update request carried a bad task count."}, // HTTP 400
    {1032, "Too many support logs are already stored to accept another."}, // HTTP 409
    {1033, "The requested support log does not exist."}, // HTTP 404
    {1034, "Too many support-log collections are already running."}, // HTTP 409
    {1035, "The device is currently offline and unreachable."}, // HTTP 400
    {1036, "The device information supplied for binding is invalid."}, // HTTP 400
    {1037, "The device limit for this licence has been reached."}, // HTTP 400
    {1038, "The bind attempt failed on a network error."}, // HTTP 400
    {1039, "The bound device never answered the handshake."}, // HTTP 400
    {1040, "The statistics query asked for a value outside the allowed range."}, // HTTP 400
    {1041, "The SDK call was made with an invalid parameter."}, // HTTP 400
    {1042, "The SDK request could not be completed by the server."}, // HTTP 500
    {1043, "The SDK login ticket presented is not valid."}, // HTTP 400
    {1043, "The SDK client is sending requests too quickly."}, // HTTP 429
    {1044, "The SDK caller is not logged in."}, // HTTP 401
    {1045, "SDK activation did not succeed."}, // HTTP 400
    {1046, "Device login through the SDK failed."}, // HTTP 400
    {1047, "No notification configuration with that identity exists."}, // HTTP 404
    {1048, "A notification configuration with that identity already exists."}, // HTTP 400
    {1049, "The operation was not executed because preconditions were not met."}, // HTTP 400
    {1050, "This operation is not supported by the server."}, // HTTP 400
    {1051, "The device is busy with other work right now."}, // HTTP 400
    {1052, "The device reported an internal fault; its SD card should be checked."}, // HTTP 400
    {1053, "The filament settings supplied are not valid."}, // HTTP 400
    {1054, "The device's hardware does not support this operation."}, // HTTP 400
    {1055, "The requested byte range lies outside the file's bounds."}, // HTTP 416
    {1056, "The username must be between 2 and 128 bytes in length."}, // HTTP 400
    {1057, "The password must mix numbers, letters and special characters, run at least 8 bytes and no more than 64."}, // HTTP 400
    {1058, "Both the date and the timestamp were left empty."}, // HTTP 400
    {1059, "The date must be formatted like 2025-01-01 15:00:00."}, // HTTP 400
    {1060, "The timestamp is out of range or is not second-granularity."}, // HTTP 400
    {1061, "The time zone value must look like UTC+08:00."}, // HTTP 400
    {1062, "An account with that identity already exists."}, // HTTP 400
    {1063, "No account matches the requested username."}, // HTTP 400
    {1063, "This account has hit its device-count ceiling."}, // HTTP 400
    {1064, "The region must be either CN or COM."}, // HTTP 400
    {1065, "The environment must be one of QA, DEV, PRE, PREUS, CN or COM."}, // HTTP 400
    {1066, "There is not enough free storage space available."}, // HTTP 400
    {1067, "The HMS model referenced is not supported."}, // HTTP 400
    {1068, "The requested HMS information was not found."}, // HTTP 400
    {1069, "The firmware is older than the minimum version this server accepts."}, // HTTP 400
    {1070, "This server is already activated."}, // HTTP 400
    {1071, "The server activation attempt failed."}, // HTTP 400
    {1072, "The extruder reports that no filament is loaded."}, // HTTP 400
    {1073, "The licence certificate is outside its valid time window."}, // HTTP 400
    {1074, "A tag with that name already exists."}, // HTTP 400
    {1075, "The folder ordering supplied does not match the expected order."}, // HTTP 400
    {1076, "A statistics export is already running."}, // HTTP 409
    {1077, "The requested statistics export does not exist."}, // HTTP 404
    {1078, "The RTSP relay server is at capacity."}, // HTTP 429
    {1079, "This device has reached its RTSP relay limit."}, // HTTP 429
    {1080, "Setting up the RTSP relay timed out."}, // HTTP 408
    {1081, "The RTSP relay failed to initialize."}, // HTTP 502
    {10001, "The caller lacks permission for this action."}, // HTTP 403
    {10002, "The upstream service returned nothing at all."}, // HTTP 500
    {10003, "The upstream service answered with a negative response."}, // HTTP 500
    {84033543, "The printer rejected an MQTT command because its "
               "per-command signature was missing or invalid."}, // MQTT command-security
    {84033545, "The printer rejected a command because its signature "
               "or task-id did not verify."}, // MQTT command-security
};

const std::size_t kXerrRowCount = sizeof(kXerrRows) / sizeof(kXerrRows[0]);

// Binary search over one of the two sorted generated tables
// (src/err_table_data.cpp). Empty string on miss.
std::string find_message(const data::Entry* entries, std::size_t count,
                         const std::string& key)
{
    const data::Entry* const begin = entries;
    const data::Entry* const end = entries + count;
    const data::Entry* it = std::lower_bound(
        begin, end, key,
        [](const data::Entry& e, const std::string& k) {
            return std::strcmp(e.key, k.c_str()) < 0;
        });
    if (it != end && key == it->key) return it->message;
    return std::string();
}

// Normalize one attr/code fragment (string side of a wire pair) to
// 8-lowercase-hex. Empty if malformed or wider than 8 hex digits.
std::string hex8(const std::string& raw)
{
    std::string out;
    out.reserve(8);
    for (char ch : raw) {
        if (ch == '-' || ch == '_') continue;
        if (ch >= '0' && ch <= '9') { out.push_back(ch); continue; }
        if (ch >= 'A' && ch <= 'F') { out.push_back(static_cast<char>(ch - 'A' + 'a')); continue; }
        if (ch >= 'a' && ch <= 'f') { out.push_back(ch); continue; }
        return std::string();
    }
    if (out.empty() || out.size() > 8) return std::string();
    out.insert(0, 8 - out.size(), '0');
    return out;
}

// One emitted decode line: raw code is always present; "(unknown)" marks
// a lookup miss without hiding the code.
std::string make_line(const std::string& prefix, const std::string& raw,
                      const std::string& message)
{
    return prefix + raw + " -> " + (message.empty() ? "(unknown)" : message);
}

void collect_hms_array(const obn::json::Array& arr, std::vector<std::string>& out)
{
    for (const obn::json::Value& el : arr) {
        std::string key;
        if (el.is_string()) {
            // Whole-entry presentation form, e.g. "0500-0500-0001-0007".
            key = normalize_hms_key(el.as_string());
        } else if (el.is_object()) {
            const obn::json::Object& obj = el.as_object();
            const auto attr_it = obj.find("attr");
            const auto code_it = obj.find("code");
            if (attr_it == obj.end() || code_it == obj.end()) continue; // code-less entry: no line
            std::string attr_part;
            std::string code_part;
            if (attr_it->second.is_number() && code_it->second.is_number()) {
                char buf[9];
                std::snprintf(buf, sizeof(buf), "%08x",
                              static_cast<std::uint32_t>(attr_it->second.as_int()));
                attr_part = buf;
                std::snprintf(buf, sizeof(buf), "%08x",
                              static_cast<std::uint32_t>(code_it->second.as_int()));
                code_part = buf;
            } else if (attr_it->second.is_string() && code_it->second.is_string()) {
                attr_part = hex8(attr_it->second.as_string());
                code_part = hex8(code_it->second.as_string());
            } else {
                continue; // unusable pair: no line
            }
            if (attr_part.empty() || code_part.empty()) continue;
            key = attr_part + code_part;
        } else {
            continue; // non-code element: no line
        }
        if (key.empty()) continue;
        out.push_back(make_line("hms code=", key, describe_hms(key)));
    }
}

void scan_value(const obn::json::Value& v, std::vector<std::string>& out)
{
    if (v.is_object()) {
        // std::map iteration = lexicographic key order (the specified,
        // stable traversal order for scan_frame_codes).
        for (const auto& kv : v.as_object()) {
            const std::string& key = kv.first;
            const obn::json::Value& val = kv.second;
            if (key == "hms" && val.is_array()) {
                collect_hms_array(val.as_array(), out);
            } else if ((key == "err_code" || key == "print_error") &&
                       (val.is_number() || val.is_string())) {
                const std::string raw = val.is_number()
                    ? std::to_string(val.as_int())
                    : val.as_string();
                out.push_back(make_line("err_code ", raw, describe_err_code(raw)));
            }
            if (val.is_object() || val.is_array()) scan_value(val, out);
        }
    } else if (v.is_array()) {
        // Array elements in wire order.
        for (const obn::json::Value& el : v.as_array()) scan_value(el, out);
    }
}

} // namespace

std::string normalize_hms_key(const std::string& raw)
{
    std::string key;
    key.reserve(16);
    for (char ch : raw) {
        if (ch == '-' || ch == '_') continue;
        if (ch >= '0' && ch <= '9') { key.push_back(ch); continue; }
        if (ch >= 'A' && ch <= 'F') { key.push_back(static_cast<char>(ch - 'A' + 'a')); continue; }
        if (ch >= 'a' && ch <= 'f') { key.push_back(ch); continue; }
        return std::string(); // malformed character
    }
    if (key.size() != 16) return std::string();
    return key;
}

std::string hms_key(std::uint32_t attr, std::uint32_t code)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%08x%08x", attr, code);
    return std::string(buf);
}

std::string describe_hms(const std::string& any_format_code)
{
    const std::string key = normalize_hms_key(any_format_code);
    if (key.empty()) return std::string();
    // Lazy function-local-static bounds over the sorted generated table.
    static const data::Entry* const begin = data::hms_entries;
    static const data::Entry* const end = begin + data::hms_count;
    const data::Entry* it = std::lower_bound(
        begin, end, key,
        [](const data::Entry& e, const std::string& k) {
            return std::strcmp(e.key, k.c_str()) < 0;
        });
    if (it != end && key == it->key) return it->message;
    return std::string();
}

std::string describe_err_code(const std::string& code)
{
    if (code.empty()) return std::string();

    bool all_digits = true;
    for (char ch : code) {
        if (ch < '0' || ch > '9') { all_digits = false; break; }
    }

    if (all_digits) {
        // Family 1 (first): the xerr map, by numeric code. Reused codes
        // join every adjacent row's paraphrase in source-row order.
        char* endp = nullptr;
        const unsigned long value = std::strtoul(code.c_str(), &endp, 10);
        if (endp != nullptr && *endp == '\0') {
            static const XerrRow* const x_begin = kXerrRows;
            static const XerrRow* const x_end = kXerrRows + kXerrRowCount;
            const XerrRow* it = std::lower_bound(
                x_begin, x_end, value,
                [](const XerrRow& r, unsigned long v) { return r.code < v; });
            if (it != x_end && it->code == value) {
                std::string joined = it->message;
                for (const XerrRow* more = it + 1;
                     more != x_end && more->code == value; ++more) {
                    joined += "; ";
                    joined += more->message;
                }
                return joined;
            }
            // Family 2 (second): decimal -> 8-lowercase-hex, then the
            // print_error map. No natural overlap with family 1 exists
            // in the data (swept at plan time), so when both families
            // would accept a code the xerr answer above is what callers
            // see; the order is what the adjacency pin relies on.
            if (value <= 0xFFFFFFFFul) {
                char hex[9];
                std::snprintf(hex, sizeof(hex), "%08lx", value);
                const std::string msg =
                    find_message(data::print_error_entries, data::print_error_count, hex);
                if (!msg.empty()) return msg;
            }
        }
        return std::string();
    }

    // Hex presentation: "0x"-prefixed or carrying a-f letters is read as
    // hex directly against the print_error map.
    std::string hex = code;
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex = hex.substr(2);
    if (hex.empty() || hex.size() > 8) return std::string();
    for (char& ch : hex) {
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    }
    for (char ch : hex) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return std::string(); // malformed
    }
    hex.insert(0, 8 - hex.size(), '0');
    return find_message(data::print_error_entries, data::print_error_count, hex);
}

std::size_t xerr_entry_count() { return kXerrRowCount; }
std::size_t hms_entry_count() { return data::hms_count; }
std::size_t print_error_entry_count() { return data::print_error_count; }

std::vector<std::string> scan_frame_codes(const std::string& json)
{
    std::vector<std::string> out;
    // Parses a local copy; the caller's bytes are never touched.
    const std::optional<obn::json::Value> parsed = obn::json::parse(json);
    if (!parsed) return out; // malformed JSON -> no lines
    scan_value(*parsed, out);
    return out;
}

} // namespace obn::err
